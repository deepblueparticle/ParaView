/*=========================================================================

  Program:   ParaView
  Module:    $RCSfile$

  Copyright (c) Kitware, Inc.
  All rights reserved.
  See Copyright.txt or http://www.paraview.org/HTML/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkSMSessionClient.h"

#include "vtkClientServerStream.h"
#include "vtkCommand.h"
#include "vtkMPIMToNSocketConnectionPortInformation.h"
#include "vtkMultiProcessController.h"
#include "vtkMultiProcessStream.h"
#include "vtkNetworkAccessManager.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkProcessModule.h"
#include "vtkPVConfig.h"
#include "vtkPVMultiClientsInformation.h"
#include "vtkPVServerInformation.h"
#include "vtkPVSessionServer.h"
#include "vtkReservedRemoteObjectIds.h"
#include "vtkSMCollaborationManager.h"
#include "vtkSMMessage.h"
#include "vtkSMPropertyHelper.h"
#include "vtkSMProxy.h"
#include "vtkSMProxyLocator.h"
#include "vtkSMProxyManager.h"
#include "vtkSMProxyProperty.h"
#include "vtkSMServerStateLocator.h"
#include "vtkSMSessionProxyManager.h"
#include "vtkSocketCommunicator.h"

#include <string>
#include <vtksys/ios/sstream>
#include <vtksys/RegularExpression.hxx>

#include <assert.h>
#include <set>

#define UPDATE_VTK_OBJECTS(object)                               \
  if(vtkSMProxy::SafeDownCast(object))                           \
    vtkSMProxy::SafeDownCast(object)->UpdateVTKObjects();

//****************************************************************************/
//                    Internal Classes and typedefs
//****************************************************************************/
namespace
{
  void RMICallback(void *localArg,
    void *remoteArg, int remoteArgLength, int vtkNotUsed(remoteProcessId))
    {
    vtkSMSessionClient* self = reinterpret_cast<vtkSMSessionClient*>(localArg);
    self->OnServerNotificationMessageRMI(remoteArg, remoteArgLength);
    }
};
//****************************************************************************/
vtkStandardNewMacro(vtkSMSessionClient);
vtkCxxSetObjectMacro(vtkSMSessionClient, RenderServerController,
                     vtkMultiProcessController);
vtkCxxSetObjectMacro(vtkSMSessionClient, DataServerController,
                     vtkMultiProcessController);
//----------------------------------------------------------------------------
vtkSMSessionClient::vtkSMSessionClient() : Superclass(false)
{
  // Init global Ids
  this->LastGlobalID = this->LastGlobalIDAvailable = 0;

  // This session can only be created on the client.
  this->RenderServerController = NULL;
  this->DataServerController = NULL;
  this->URI = NULL;
  this->CollaborationCommunicator = NULL;
  this->AbortConnect = false;

  this->DataServerInformation = vtkPVServerInformation::New();
  this->RenderServerInformation = vtkPVServerInformation::New();
  this->ServerInformation = vtkPVServerInformation::New();
  this->ServerLastInvokeResult = new vtkClientServerStream();

  // Register server state locator for that specific session
  vtkNew<vtkSMServerStateLocator> serverStateLocator;
  serverStateLocator->SetSession(this);
  this->GetStateLocator()->SetParentLocator(serverStateLocator.GetPointer());

  // Default value
  this->NoMoreDelete = false;
  this->NotBusy = 0;
}

//----------------------------------------------------------------------------
vtkSMSessionClient::~vtkSMSessionClient()
{
  if(this->DataServerController)
    {
    this->DataServerController->RemoveAllRMICallbacks(
        vtkPVSessionServer::SERVER_NOTIFICATION_MESSAGE_RMI);
    }
  if (this->GetIsAlive())
    {
    this->CloseSession();
    }
  this->SetRenderServerController(0);
  this->SetDataServerController(0);
  this->DataServerInformation->Delete();
  this->RenderServerInformation->Delete();
  this->ServerInformation->Delete();
  if(this->CollaborationCommunicator)
    {
    this->CollaborationCommunicator->Delete();
    this->CollaborationCommunicator = NULL;
    }
  this->SetURI(0);

  delete this->ServerLastInvokeResult;
  this->ServerLastInvokeResult = NULL;
}

//----------------------------------------------------------------------------
vtkMultiProcessController* vtkSMSessionClient::GetController(ServerFlags processType)
{
  switch (processType)
    {
  case CLIENT:
    return NULL;

  case DATA_SERVER:
  case DATA_SERVER_ROOT:
    return this->DataServerController;

  case RENDER_SERVER:
  case RENDER_SERVER_ROOT:
    return (this->RenderServerController? this->RenderServerController :
      this->DataServerController);

  default:
    vtkWarningMacro("Invalid processtype of GetController(): " << processType);
    }

  return NULL;
}

//----------------------------------------------------------------------------
bool vtkSMSessionClient::Connect(const char* url)
{
  this->SetURI(url);
  vtksys::RegularExpression pvserver("^cs://([^:]+)(:([0-9]+))?");
  vtksys::RegularExpression pvserver_reverse ("^csrc://([^:]+)?(:([0-9]+))?");

  vtksys::RegularExpression pvrenderserver(
    "^cdsrs://([^:]+):([0-9]+)/([^:]+):([0-9]+)");
  vtksys::RegularExpression pvrenderserver_reverse (
    "^cdsrsrc://(([^:]+)?(:([0-9]+))?/([^:]+)?(:([0-9]+))?)?");

  vtksys_ios::ostringstream handshake;
  handshake << "handshake=paraview." << PARAVIEW_VERSION;
  // Add connect-id if needed (or maybe we extract that from url as well
  // (just like vtkNetworkAccessManager).

  std::string data_server_url;
  std::string render_server_url;

  if (pvserver.find(url))
    {
    std::string hostname = pvserver.match(1);
    int port = atoi(pvserver.match(3).c_str());
    port = (port <= 0)? 11111: port;

    vtksys_ios::ostringstream stream;
    stream << "tcp://" << hostname << ":" << port << "?" << handshake.str();
    data_server_url = stream.str();
    }
  else if (pvserver_reverse.find(url))
    {
    // 0 ports are acceptable for reverse connections.
    int port = atoi(pvserver_reverse.match(3).c_str());
    port = (port < 0)? 11111: port;
    vtksys_ios::ostringstream stream;
    stream << "tcp://localhost:" << port << "?listen=true&nonblocking=true&" << handshake.str();
    data_server_url = stream.str();

    }
  else if (pvrenderserver.find(url))
    {
    std::string dataserverhost = pvrenderserver.match(1);
    int dsport = atoi(pvrenderserver.match(2).c_str());
    dsport = (dsport <= 0)? 11111 : dsport;

    std::string renderserverhost = pvrenderserver.match(3);
    int rsport = atoi(pvrenderserver.match(4).c_str());
    rsport = (rsport <= 0)? 22221 : rsport;

    vtksys_ios::ostringstream stream;
    stream << "tcp://" << dataserverhost << ":" << dsport
      << "?" << handshake.str();
    data_server_url = stream.str().c_str();

    vtksys_ios::ostringstream stream2;
    stream2 << "tcp://" << renderserverhost << ":" << rsport
      << "?" << handshake.str();
    render_server_url = stream2.str();
    }
  else if (pvrenderserver_reverse.find(url))
    {
    // 0 ports are acceptable for reverse connections.
    int dsport = atoi(pvrenderserver_reverse.match(4).c_str());
    dsport = (dsport < 0)? 11111 : dsport;
    int rsport = atoi(pvrenderserver_reverse.match(7).c_str());
    rsport = (rsport < 0)? 22221 : rsport;

    vtksys_ios::ostringstream stream;
    stream << "tcp://localhost:" << dsport
      << "?listen=true&nonblocking=true&" << handshake.str();
    data_server_url = stream.str().c_str();

    stream.clear();
    stream << "tcp://localhost:" << rsport
      << "?listen=true&nonblocking=true&" << handshake.str();
    render_server_url = stream.str();
    }

  bool need_rcontroller = render_server_url.size() > 0;
  vtkNetworkAccessManager* nam =
    vtkProcessModule::GetProcessModule()->GetNetworkAccessManager();
  vtkMultiProcessController* dcontroller =
    nam->NewConnection(data_server_url.c_str());
  vtkMultiProcessController* rcontroller = need_rcontroller?
    nam->NewConnection(render_server_url.c_str()) : NULL;

  this->AbortConnect = false;
  while (!this->AbortConnect &&
    (dcontroller == NULL || (need_rcontroller && rcontroller == NULL)))
    {
    int result = nam->ProcessEvents(100);
    if (result == 1) // some activity
      {
      dcontroller = dcontroller? dcontroller :
        nam->NewConnection(data_server_url.c_str());
      rcontroller = (rcontroller || !need_rcontroller)? rcontroller :
        nam->NewConnection(render_server_url.c_str());
      }
    else if (result == 0) // timeout
      {
      double foo=0.5;
      this->InvokeEvent(vtkCommand::ProgressEvent, &foo);
      }
    else if (result == -1)
      {
      vtkErrorMacro("Some error in socket processing.");
      break;
      }
    }
  if (dcontroller)
    {
    this->SetDataServerController(dcontroller);
    dcontroller->GetCommunicator()->AddObserver(
        vtkCommand::WrongTagEvent, this, &vtkSMSessionClient::OnWrongTagEvent);
    dcontroller->AddRMICallback( &RMICallback, this,
                                 vtkPVSessionServer::SERVER_NOTIFICATION_MESSAGE_RMI);
    dcontroller->Delete();
    }
  if (rcontroller)
    {
    this->SetRenderServerController(rcontroller);
    rcontroller->GetCommunicator()->AddObserver(
        vtkCommand::WrongTagEvent, this, &vtkSMSessionClient::OnWrongTagEvent);
    rcontroller->Delete();
    }

  bool success = (this->DataServerController && (!need_rcontroller||
      this->RenderServerController));

  if (success)
    {
    this->GatherInformation(vtkPVSession::DATA_SERVER_ROOT,
      this->DataServerInformation, 0);
    this->GatherInformation(vtkPVSession::RENDER_SERVER_ROOT,
      this->RenderServerInformation, 0);

    // Keep the combined server information to return when
    // GetServerInformation() is called.
    this->ServerInformation->AddInformation(this->RenderServerInformation);
    this->ServerInformation->AddInformation(this->DataServerInformation);

    // Initializes other things like plugin manager/proxy-manager etc.
    this->Initialize();
    }

  // TODO: test with following expressions.
  // vtkSMSessionClient::Connect("cs://localhost");
  // vtkSMSessionClient::Connect("cs://localhost:2212");
  // vtkSMSessionClient::Connect("csrc://:2212");
  // vtkSMSessionClient::Connect("csrc://");
  // vtkSMSessionClient::Connect("csrc://localhost:2212");


  // vtkSMSessionClient::Connect("cdsrs://localhost/localhost");
  // vtkSMSessionClient::Connect("cdsrs://localhost:99999/localhost");
  // vtkSMSessionClient::Connect("cdsrs://localhost/localhost:99999");
  // vtkSMSessionClient::Connect("cdsrs://localhost:66666/localhost:99999");

  // vtkSMSessionClient::Connect("cdsrsrc://");
  // vtkSMSessionClient::Connect("cdsrsrc://localhost:2212/:23332");
  // vtkSMSessionClient::Connect("cdsrsrc://:2212/:23332");
  // vtkSMSessionClient::Connect("cdsrsrc:///:23332");
  return success;
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::Initialize()
{
  this->Superclass::Initialize();

  // Setup the socket connnection between data-server and render-server.
  if (this->DataServerController && this->RenderServerController)
    {
    this->SetupDataServerRenderServerConnection();
    }
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::SetupDataServerRenderServerConnection()
{
  vtkSMSessionProxyManager* pxm =
      vtkSMProxyManager::GetProxyManager()->GetSessionProxyManager(this);
  vtkSMProxy* mpiMToN = pxm->NewProxy("internals", "MPIMToNSocketConnection");
  vtkSMPropertyHelper(mpiMToN, "WaitingProcess").Set(
    vtkProcessModule::PROCESS_RENDER_SERVER);
  mpiMToN->UpdateVTKObjects();

  vtkMPIMToNSocketConnectionPortInformation* info =
    vtkMPIMToNSocketConnectionPortInformation::New();
  this->GatherInformation(RENDER_SERVER, info, mpiMToN->GetGlobalID());

  vtkSMPropertyHelper helper(mpiMToN, "Connections");
  for (int cc = 0; cc < info->GetNumberOfConnections(); cc++)
    {
    vtksys_ios::ostringstream processNo;
    processNo << cc;
    vtksys_ios::ostringstream str;
    str << info->GetProcessPort(cc);
    helper.Set(3*cc, processNo.str().c_str());
    helper.Set(3*cc+1, str.str().c_str());
    helper.Set(3*cc+2, info->GetProcessHostName(cc));
    }
  mpiMToN->UpdateVTKObjects();
  info->Delete();
  info = NULL;

  vtkClientServerStream stream;
  stream << vtkClientServerStream::Invoke
            << vtkClientServerID(1) // ID for vtkSMSessionCore helper.
            << "SetMPIMToNSocketConnection"
            << VTKOBJECT(mpiMToN)
            << vtkClientServerStream::End;
  this->ExecuteStream(vtkPVSession::SERVERS, stream);

  // the proxy can now be destroyed.
  mpiMToN->Delete();
}

//----------------------------------------------------------------------------
bool vtkSMSessionClient::GetIsAlive()
{
  // TODO: add check to test connection existence.
  return (this->DataServerController != NULL);
}

namespace
{
  template <class T>
  T vtkMax(const T& a, const T& b) { return (a < b)? b: a; }
};

//----------------------------------------------------------------------------
int vtkSMSessionClient::GetNumberOfProcesses(vtkTypeUInt32 servers)
{
  int num_procs = 0;
  if (servers & vtkPVSession::CLIENT)
    {
    num_procs = vtkMax(num_procs,
      this->Superclass::GetNumberOfProcesses(servers));
    }
  if (servers & vtkPVSession::DATA_SERVER ||
    servers & vtkPVSession::DATA_SERVER_ROOT)
    {
    num_procs = vtkMax(num_procs,
      this->DataServerInformation->GetNumberOfProcesses());
    }

  if (servers & vtkPVSession::RENDER_SERVER ||
    servers & vtkPVSession::RENDER_SERVER_ROOT)
    {
    num_procs = vtkMax(num_procs,
      this->RenderServerInformation->GetNumberOfProcesses());
    }

  return num_procs;
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::CloseSession()
{
  if (this->DataServerController)
    {
    this->DataServerController->TriggerRMIOnAllChildren(
      vtkPVSessionServer::CLOSE_SESSION);
    vtkSocketCommunicator::SafeDownCast(
      this->DataServerController->GetCommunicator())->CloseConnection();
    this->SetDataServerController(0);
    }
  if (this->RenderServerController)
    {
    this->RenderServerController->TriggerRMIOnAllChildren(
      vtkPVSessionServer::CLOSE_SESSION);
    vtkSocketCommunicator::SafeDownCast(
      this->RenderServerController->GetCommunicator())->CloseConnection();
    this->SetRenderServerController(0);
    }
}
//----------------------------------------------------------------------------
void vtkSMSessionClient::PreDisconnection()
{
  this->NoMoreDelete = true;
}

//----------------------------------------------------------------------------
vtkTypeUInt32 vtkSMSessionClient::GetRealLocation(vtkTypeUInt32 location)
{
  if (this->RenderServerController == NULL)
    {
    // re-route all render-server messages to data-server.
    if ((location & vtkPVSession::RENDER_SERVER) != 0)
      {
      location |= vtkPVSession::DATA_SERVER;
      location &= ~vtkPVSession::RENDER_SERVER;
      }
    if ((location & vtkPVSession::RENDER_SERVER_ROOT) != 0)
      {
      location |= vtkPVSession::DATA_SERVER_ROOT;
      location &= ~vtkPVSession::RENDER_SERVER_ROOT;
      }
    }
  return location;
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::PushState(vtkSMMessage* message)
{
  // Prevent to push anything during the Quit process
  if(this->NoMoreDelete)
    {
    return;
    }

  vtkTypeUInt32 location = this->GetRealLocation(message->location());
  message->set_location(location);
  int num_controllers=0;
  vtkMultiProcessController* controllers[2] = {NULL, NULL};

  if ( (location &
        (vtkPVSession::DATA_SERVER|vtkPVSession::DATA_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->DataServerController;
    }
  if ((location &
       (vtkPVSession::RENDER_SERVER|vtkPVSession::RENDER_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->RenderServerController;
    }
  if (num_controllers > 0)
    {
    vtkMultiProcessStream stream;
    stream << static_cast<int>(vtkPVSessionServer::PUSH);
    stream << message->SerializeAsString();
    std::vector<unsigned char> raw_message;
    stream.GetRawData(raw_message);
    for (int cc=0; cc < num_controllers; cc++)
      {
      controllers[cc]->TriggerRMIOnAllChildren(
          &raw_message[0], static_cast<int>(raw_message.size()),
          vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);
      }
    }

  if ((location & vtkPVSession::CLIENT) != 0)
    {
    this->Superclass::PushState(message);

    // For collaboration purpose we might need to share the proxy state with
    // other clients
    if( num_controllers == 0 && this->IsMultiClients())
      {
      vtkSMRemoteObject* remoteObject =
          vtkSMRemoteObject::SafeDownCast(this->GetRemoteObject(message->global_id()));
      vtkSMMessage msg;
      if(remoteObject && remoteObject->GetFullState() == NULL)
        {
        vtkWarningMacro( "The following vtkRemoteObject ("
                         << remoteObject->GetClassName() << "-" << remoteObject->GetGlobalIDAsString()
                         << ") does not support properly GetFullState() so no "
                         << "collaboration mechanisme could be applied to it.");
        }
      else if(remoteObject && !remoteObject->IsLocalPushOnly())
        {
        msg.CopyFrom( remoteObject ? *remoteObject->GetFullState(): *message);
        msg.set_global_id(message->global_id());
        msg.set_location(message->location());

        // Add extra-informations
        msg.set_share_only(true);
        msg.set_client_id(this->ServerInformation->GetClientId());

        vtkMultiProcessStream stream;
        stream << static_cast<int>(vtkPVSessionServer::PUSH);
        stream << msg.SerializeAsString();
        std::vector<unsigned char> raw_message;
        stream.GetRawData(raw_message);
        this->DataServerController->TriggerRMIOnAllChildren(
            &raw_message[0], static_cast<int>(raw_message.size()),
            vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);
        }
      else if(!remoteObject)
        {
        // This issue seems to happen only sometime on amber12 in collaboration
        // and are hard to reproduce
        // If we get time to figure out how this situation happen and why that
        // would be nice but for now, we'll just keep a warning arround as
        // this case is harmless.
        vtkWarningMacro( "No remote object found for corresponding state: "
                         << message->global_id());
        message->PrintDebugString();
        }
      }
    }
  else
    {
    // We do not execute anything locally we just keep track
    // of the State History for Undo/Redo
    this->UpdateStateHistory(message);
    }
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::PullState(vtkSMMessage* message)
{
  this->StartBusyWork();
  vtkTypeUInt32 location = this->GetRealLocation(message->location());
  message->set_location(location);

  vtkMultiProcessController* controller = NULL;

  // We make sure that only ONE location is targeted with a priority order
  // (1) Client (2) DataServer (3) RenderServer
  if ( (location & vtkPVSession::CLIENT) != 0)
    {
    controller = NULL;
    }
  else if ( (location & 
      (vtkPVSession::DATA_SERVER | vtkPVSession::DATA_SERVER_ROOT)) != 0)
    {
    controller = this->DataServerController;
    }
  else if ( (location & 
      (vtkPVSession::RENDER_SERVER | vtkPVSession::RENDER_SERVER_ROOT)) != 0)
    {
    controller = this->RenderServerController;
    }

  if (controller)
    {
    vtkMultiProcessStream stream;
    stream << static_cast<int>(vtkPVSessionServer::PULL);
    stream << message->SerializeAsString();
    std::vector<unsigned char> raw_message;
    stream.GetRawData(raw_message);
    controller->TriggerRMIOnAllChildren(
      &raw_message[0], static_cast<int>(raw_message.size()),
      vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);

    // Get the reply
    vtkMultiProcessStream replyStream;
    controller->Receive(replyStream, 1, vtkPVSessionServer::REPLY_PULL);
    std::string string;
    replyStream >> string;
    message->ParseFromString(string);
    }
  else
    {
    this->Superclass::PullState(message);
    // Everything is local no communication needed (Send/Reply)
    }
  this->EndBusyWork();
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::ExecuteStream(
  vtkTypeUInt32 location, const vtkClientServerStream& cssstream,
  bool ignore_errors)
{
  // Prevent to push anything during the Quit process
  if(this->NoMoreDelete)
    {
    return;
    }

  location = this->GetRealLocation(location);

  vtkMultiProcessController* controllers[2] = {NULL, NULL};
  int num_controllers=0;
  if ((location &
      (vtkPVSession::DATA_SERVER|vtkPVSession::DATA_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->DataServerController;
    }
  if ((location &
    (vtkPVSession::RENDER_SERVER|vtkPVSession::RENDER_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->RenderServerController;
    }

  if ( num_controllers > 0)
    {
    const unsigned char* data;
    size_t size;
    cssstream.GetData(&data, &size);

    vtkMultiProcessStream stream;
    stream << static_cast<int>(vtkPVSessionServer::EXECUTE_STREAM)
      << static_cast<int>(ignore_errors) << static_cast<int>(size);
    std::vector<unsigned char> raw_message;
    stream.GetRawData(raw_message);

    for (int cc=0; cc < num_controllers; cc++)
      {
      controllers[cc]->TriggerRMIOnAllChildren(
        &raw_message[0], static_cast<int>(raw_message.size()),
        vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);
      controllers[cc]->Send(data, static_cast<int>(size), 1,
        vtkPVSessionServer::EXECUTE_STREAM_TAG);
      }
    }

  if ( (location & vtkPVSession::CLIENT) != 0)
    {
    this->Superclass::ExecuteStream(location, cssstream, ignore_errors);
    }
}

//----------------------------------------------------------------------------
const vtkClientServerStream& vtkSMSessionClient::GetLastResult(vtkTypeUInt32 location)
{
  this->StartBusyWork();
  location = this->GetRealLocation(location);

  vtkMultiProcessController* controller = NULL;
  if (location & vtkPVSession::CLIENT)
    {
    controller = NULL;
    }
  else if ( (location & vtkPVSession::DATA_SERVER_ROOT) ||
    (location & vtkPVSession::DATA_SERVER) )
    {
    controller = this->DataServerController;
    }
  else if ( (location  & vtkPVSession::RENDER_SERVER_ROOT) ||
    (location & vtkPVSession::RENDER_SERVER) )
    {
    controller = this->RenderServerController;
    }

  if (controller)
    {
    this->ServerLastInvokeResult->Reset();

    vtkMultiProcessStream stream;
    stream << static_cast<int>(vtkPVSessionServer::LAST_RESULT);
    std::vector<unsigned char> raw_message;
    stream.GetRawData(raw_message);
    controller->TriggerRMIOnAllChildren(
      &raw_message[0], static_cast<int>(raw_message.size()),
      vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);

    // Get the reply
    int size=0;
    controller->Receive(&size, 1, 1, vtkPVSessionServer::REPLY_LAST_RESULT);
    unsigned char* raw_data = new unsigned char[size+1];
    controller->Receive(raw_data, size, 1, vtkPVSessionServer::REPLY_LAST_RESULT);
    this->ServerLastInvokeResult->SetData(raw_data, size);
    delete [] raw_data;
    this->EndBusyWork();
    return *this->ServerLastInvokeResult;
    }

  this->EndBusyWork();
  return this->Superclass::GetLastResult(location);
}

//----------------------------------------------------------------------------
bool vtkSMSessionClient::GatherInformation(
  vtkTypeUInt32 location, vtkPVInformation* information, vtkTypeUInt32 globalid)
{
  this->StartBusyWork();
  if (this->RenderServerController == NULL)
    {
    // re-route all render-server messages to data-server.
    if (location & vtkPVSession::RENDER_SERVER)
      {
      location |= vtkPVSession::DATA_SERVER;
      location &= ~vtkPVSession::RENDER_SERVER;
      }
    if (location & vtkPVSession::RENDER_SERVER_ROOT)
      {
      location |= vtkPVSession::DATA_SERVER_ROOT;
      location &= ~vtkPVSession::RENDER_SERVER_ROOT;
      }
    }

  bool add_local_info = false;
  if ( (location & vtkPVSession::CLIENT) != 0)
    {
    bool ret_value = this->Superclass::GatherInformation(
      location, information, globalid);
    if (information->GetRootOnly())
      {
      this->EndBusyWork();
      return ret_value;
      }
    add_local_info = true;
    }

  vtkMultiProcessStream stream;
  stream << static_cast<int>(vtkPVSessionServer::GATHER_INFORMATION)
    << location
    << information->GetClassName()
    << globalid;
  information->CopyParametersToStream(stream);
  std::vector<unsigned char> raw_message;
  stream.GetRawData(raw_message);

  vtkMultiProcessController* controller = NULL;

  if ( (location & vtkPVSession::DATA_SERVER) != 0 ||
    (location & vtkPVSession::DATA_SERVER_ROOT) != 0)
    {
    controller = this->DataServerController;
    }

  else if (this->RenderServerController != NULL &&
    ((location & vtkPVSession::RENDER_SERVER) != 0 ||
    (location & vtkPVSession::RENDER_SERVER_ROOT) != 0))
    {
    controller = this->RenderServerController;
    }

  if (controller)
    {
    controller->TriggerRMIOnAllChildren(
      &raw_message[0], static_cast<int>(raw_message.size()),
      vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);

    int length2 = 0;
    controller->Receive(&length2, 1, 1, vtkPVSessionServer::REPLY_GATHER_INFORMATION_TAG);
    if (length2 <= 0)
      {
      vtkErrorMacro("Server failed to gather information.");
      this->EndBusyWork();
      return false;
      }
    unsigned char* data2 = new unsigned char[length2];
    if (!controller->Receive((char*)data2, length2, 1,
        vtkPVSessionServer::REPLY_GATHER_INFORMATION_TAG))
      {
      vtkErrorMacro("Failed to receive information correctly.");
      delete [] data2;
      this->EndBusyWork();
      return false;
      }
    vtkClientServerStream csstream;
    csstream.SetData(data2, length2);
    if (add_local_info)
      {
      vtkPVInformation* tempInfo = information->NewInstance();
      tempInfo->CopyFromStream(&csstream);
      information->AddInformation(tempInfo);
      tempInfo->Delete();
      }
    else
      {
      information->CopyFromStream(&csstream);
      }
    delete [] data2;
    }
  this->EndBusyWork();
  return false;
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::UnRegisterSIObject(vtkSMMessage* message)
{
  if(this->NoMoreDelete)
    {
    return;
    }

  vtkTypeUInt32 location = this->GetRealLocation(message->location());
  message->set_location(location);
  message->set_client_id(this->GetServerInformation()->GetClientId());

  vtkMultiProcessController* controllers[2] = {NULL, NULL};
  int num_controllers=0;
  if ((location &
      (vtkPVSession::DATA_SERVER|vtkPVSession::DATA_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->DataServerController;
    }
  if ((location &
    (vtkPVSession::RENDER_SERVER|vtkPVSession::RENDER_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->RenderServerController;
    }
  if (num_controllers > 0)
    {
    vtkMultiProcessStream stream;
    stream << static_cast<int>(vtkPVSessionServer::UNREGISTER_SI);
    stream << message->SerializeAsString();
    std::vector<unsigned char> raw_message;
    stream.GetRawData(raw_message);
    for (int cc=0; cc < num_controllers; cc++)
      {
      controllers[cc]->TriggerRMIOnAllChildren(
        &raw_message[0], static_cast<int>(raw_message.size()),
        vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);
      }
    }

  if  ( (location & vtkPVSession::CLIENT) != 0)
    {
    this->Superclass::UnRegisterSIObject(message);
    }
}
//----------------------------------------------------------------------------
void vtkSMSessionClient::RegisterSIObject(vtkSMMessage* message)
{
  if(this->NoMoreDelete)
    {
    return;
    }

  vtkTypeUInt32 location = this->GetRealLocation(message->location());
  message->set_location(location);
  message->set_client_id(this->GetServerInformation()->GetClientId());

  vtkMultiProcessController* controllers[2] = {NULL, NULL};
  int num_controllers=0;
  if ((location &
      (vtkPVSession::DATA_SERVER|vtkPVSession::DATA_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->DataServerController;
    }
  if ((location &
    (vtkPVSession::RENDER_SERVER|vtkPVSession::RENDER_SERVER_ROOT)) != 0)
    {
    controllers[num_controllers++] = this->RenderServerController;
    }
  if (num_controllers > 0)
    {
    vtkMultiProcessStream stream;
    stream << static_cast<int>(vtkPVSessionServer::REGISTER_SI);
    stream << message->SerializeAsString();
    std::vector<unsigned char> raw_message;
    stream.GetRawData(raw_message);
    for (int cc=0; cc < num_controllers; cc++)
      {
      controllers[cc]->TriggerRMIOnAllChildren(
        &raw_message[0], static_cast<int>(raw_message.size()),
        vtkPVSessionServer::CLIENT_SERVER_MESSAGE_RMI);
      }
    }

  if  ( (location & vtkPVSession::CLIENT) != 0)
    {
    this->Superclass::RegisterSIObject(message);
    }
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
//----------------------------------------------------------------------------
vtkTypeUInt32 vtkSMSessionClient::GetNextGlobalUniqueIdentifier()
{
  return this->GetNextChunkGlobalUniqueIdentifier(1);
}

//----------------------------------------------------------------------------
vtkTypeUInt32 vtkSMSessionClient::GetNextChunkGlobalUniqueIdentifier(
  vtkTypeUInt32 chunkSize)
{
  if ( (this->LastGlobalID + chunkSize) >= this->LastGlobalIDAvailable)
    {
    // we have run out of contiguous ids, request a bunch.
    vtkTypeUInt32 chunkSizeRequest = chunkSize > 500? chunkSize : 500;
    this->LastGlobalID = this->Superclass::GetNextChunkGlobalUniqueIdentifier(chunkSizeRequest);
    this->LastGlobalIDAvailable = this->LastGlobalID + chunkSizeRequest;
    }

  vtkTypeUInt32 gid = this->LastGlobalID;
  this->LastGlobalID += chunkSize;
  return gid;
}

//----------------------------------------------------------------------------
void vtkSMSessionClient::OnServerNotificationMessageRMI(void* message, int message_length)
{
  // Setup load state context
  std::string data;
  data.append(reinterpret_cast<char*>(message), message_length);

  vtkSMMessage state;
  state.ParseFromString(data);
  vtkTypeUInt32 id = state.global_id();
 
  vtkSMRemoteObject* remoteObj =
      vtkSMRemoteObject::SafeDownCast(this->GetRemoteObject(id));

  //cout << "##########     Server notification    ##########" << endl;
  //cout << id << " = " << remoteObj << "(" << (remoteObj?
  //    remoteObj->GetClassName() : "null") << ")" << endl;
  //state.PrintDebugString();
  //cout << "###################################################" << endl;

  // ProcessingRemoteNotification = true prevent
  // "ignore_synchronization" properties to be loaded...
  // Therefore camera properties won't be shared
  if(remoteObj)
    {
    bool previousValue = this->StartProcessingRemoteNotification();
    remoteObj->EnableLocalPushOnly();
    vtkSMProxyProperty::EnableProxyCreation();
    remoteObj->LoadState(&state, this->GetProxyLocator());
    UPDATE_VTK_OBJECTS(remoteObj); // If SMProxy will call UpdateVTKObjects()
    vtkSMProxyProperty::DisableProxyCreation();
    remoteObj->DisableLocalPushOnly();
    this->StopProcessingRemoteNotification(previousValue);
    }

  // Maybe that "share_only" message could be useful for collaboration
  if(remoteObj != this->GetCollaborationManager() && state.share_only())
    {
    this->GetCollaborationManager()->LoadState(&state, this->GetProxyLocator());
    }

  // Clear cache of loaded proxy
  this->GetProxyLocator()->Clear();
}
//-----------------------------------------------------------------------------
bool vtkSMSessionClient::OnWrongTagEvent( vtkObject* obj, unsigned long event,
                                          void* calldata )
{
  int tag = -1;
  const char* data = reinterpret_cast<const char*>(calldata);
  const char* ptr = data;
  memcpy(&tag, ptr, sizeof(tag));

  // Just buffer RMI_TAG's
  if ( tag == vtkMultiProcessController::RMI_TAG ||
       tag == vtkMultiProcessController::RMI_ARG_TAG )
    {
    vtkSocketCommunicator::SafeDownCast(
        this->DataServerController->GetCommunicator())->BufferCurrentMessage();
    return true; // Need to keep trying to receive.
    }

  cout << "Wrong tag but don't know how to handle it... " << tag << endl;
  abort();

  // We was not able to handle it localy
  this->Superclass::OnWrongTagEvent(obj, event, calldata);
  return false;
}

//-----------------------------------------------------------------------------
bool vtkSMSessionClient::IsNotBusy()
{
  return (this->NotBusy == 0);
}

//-----------------------------------------------------------------------------
void vtkSMSessionClient::StartBusyWork()
{
  ++this->NotBusy;
}

//-----------------------------------------------------------------------------
void vtkSMSessionClient::EndBusyWork()
{
  --this->NotBusy;
}
//-----------------------------------------------------------------------------
vtkSMCollaborationManager* vtkSMSessionClient::GetCollaborationManager()
{
  if(this->CollaborationCommunicator == NULL)
    {
    this->CollaborationCommunicator = vtkSMCollaborationManager::New();
    this->CollaborationCommunicator->SetSession(this);
    }
  return this->CollaborationCommunicator;
}