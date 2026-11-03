# WONAPI in `StarFleetOP.exe` vs `$SFCOP_ROOT/work/src/WON/ world-opponent-network/wonapi`

Source files scanned: 1445.

## RTTI classes in namespace WONAPI

RTTI classes: 50; defined in tree: 49.

| Class | Defined in source tree |
|---|---|
| `ArithmeticException` | `WONCrypt/BigInteger.h` |
| `AsyncOp` | `WONAPI.h`, `WONCommon/AsyncOp.h`, `WONLobby/LobbyDialog.h` |
| `AsyncOpContainer` | `WONCommon/AsyncOp.h`, `WONCommon/AsyncOpContainer.h` |
| `AsyncOpWithTracker` | `WONCommon/AsyncOpTracker.h` |
| `AsyncSocket` | `WONSocket/AsyncSocket.h` |
| `Auth2Certificate` | `WONAuth/Auth2Certificate.h` |
| `AuthBase` | `WONAuth/AuthBase.h` |
| `AuthCertificate` | `WONAuth/AuthCertificate.h` |
| `AuthCertificateBase` | `WONAuth/AuthCertificateBase.h` |
| `AuthPeerData` | `WONAuth/AuthPeerData.h` |
| `AuthPubKeyBlock` | `WONAuth/AuthPubKeyBlock.h` |
| `AuthSession` | `WONAuth/AuthSession.h` |
| `BigInteger` | `WONCrypt/BigInteger.h` |
| `BlockingSocket` | `WONSocket/BlockingSocket.h` |
| `ByteBuffer` | `WONCommon/ByteBuffer.h`, `WONLobby/InitLogic.h`, `WONLobby/LanLogic.h` |
| `CloseOp` | `WONSocket/CloseOp.h` |
| `ConnectOp` | `WONSocket/ConnectOp.h` |
| `CryptTransform` | `WONAuth/CryptTransform.h` |
| `DetectFirewallOp` | `WONMisc/DetectFirewallOp.h` |
| `DirEntity` | `WONDir/DirEntity.h`, `WONLobby/LobbyServer.h` |
| `Event` | `WONCommon/CriticalSection_Linux.h`, `WONCommon/CriticalSection_Mac.h`, `WONCommon/CriticalSection_Windows.h` |
| `GetCertOp` | `WONAuth/AuthContext.h`, `WONAuth/GetCertOp.h`, `WONServer/ServerOp.h` |
| `GetDirOp` | `WONDir/GetDirOp.h` |
| `GetMOTDOp` | `WONMisc/GetMOTDOp.h` |
| `HTTPGetOp` | `WONMisc/HTTPGetOp.h` |
| `LightGetCertOp` | `WONAuth/AuthContext.h`, `WONAuth/GetCertOp.h` |
| `MsgTransform` | `WONSocket/AsyncSocket.h` |
| `PeerAuthOp` | `WONAuth/PeerAuthOp.h`, `WONServer/ServerOp.h` |
| `QueueSocket` | `WONSocket/QueueSocket.h` |
| `ReadBufferException` | `WONCommon/ReadBuffer.h` |
| `RecvBytesOp` | `WONSocket/RecvBytesOp.h` |
| `RecvCRMsgOp` | `WONSocket/RecvCRMsgOp.h` |
| `RecvChunkOp` | `WONSocket/RecvBytesOp.h` |
| `RecvMsgOp` | `WONSocket/RecvMsgOp.h` |
| `RefCount` | `WONCommon/SmartPtr.h` |
| `RegKey` | `WONCommon/RegKey.h`, `WONCommon/RegKey_Windows.h` |
| `SendBytesOp` | `WONSocket/SendBytesOp.h` |
| `SendMsgOp` | `WONSocket/SendMsgOp.h` |
| `ServerContext` | `WONLobby/LobbyMisc.h`, `WONServer/ServerContext.h` |
| `ServerOp` | `WONServer/ServerOp.h` |
| `ServerRequestOp` | `WONServer/ServerRequestOp.h` |
| `SocketOp` | `WONAPI.h`, `WONSocket/QueueSocket.h`, `WONSocket/SocketOp.h` |
| `SocketThread` | `WONAPI.h`, `WONSocket/SocketOp.h`, `WONSocket/SocketThread.h` |
| `SocketThreadEx` | `WONSocket/SocketThreadEx_Linux.h`, `WONSocket/SocketThreadEx_Windows.h` |
| `SocketThreadSimple` | `WONSocket/SocketThreadSimple.h` |
| `Thread` | `WONCommon/Thread_Linux.h`, `WONCommon/Thread_Mac.h`, `WONCommon/Thread_Windows.h` |
| `TimeoutOp` | `WONCommon/AsyncOp.h` |
| `TimerThread` | `WONCommon/AsyncOp.h`, `WONCommon/TimerThread.h` |
| `WONAPIPumpThread` | **not found** |
| `WriteBuffer` | `WONCommon/WriteBuffer.h`, `WONLobby/LobbyGame.h`, `WONLobby/LobbyPlayer.h` |

## `WS_*` status strings

In exe: 302; also present in tree: 262; missing from tree: 40.

Missing: `WS_RoutingServ_ClientAlreadyBanned`, `WS_RoutingServ_ClientAlreadyExists`, `WS_RoutingServ_ClientAlreadyInvited`, `WS_RoutingServ_ClientAlreadyMuted`, `WS_RoutingServ_ClientAlreadyRegistered`, `WS_RoutingServ_ClientAlreadySpectator`, `WS_RoutingServ_ClientBanned`, `WS_RoutingServ_ClientDoesNotExist`, `WS_RoutingServ_ClientMuted`, `WS_RoutingServ_ClientNotBanned`, `WS_RoutingServ_ClientNotInvited`, `WS_RoutingServ_ClientNotMuted`, `WS_RoutingServ_ClientRegistrationPending`, `WS_RoutingServ_FailureStart`, `WS_RoutingServ_GroupAlreadyExists`, `WS_RoutingServ_GroupDoesNotExist`, `WS_RoutingServ_GroupIsPermanent`, `WS_RoutingServ_HostAlreadyExists`, `WS_RoutingServ_HostDoesNotExist`, `WS_RoutingServ_HostRefused`, `WS_RoutingServ_HostResponseTimeout`, `WS_RoutingServ_InvalidPassword`, `WS_RoutingServ_MustBeClient`, `WS_RoutingServ_MustBeHost`, `WS_RoutingServ_MustBeHostOrModerator`, `WS_RoutingServ_MustBeHostOrSuccessor`, `WS_RoutingServ_NotEligible`, `WS_RoutingServ_NotValidOnHost`, `WS_RoutingServ_ObjectAlreadyExists`, `WS_RoutingServ_ObjectDoesNotExist`, `WS_RoutingServ_ObjectTooLarge`, `WS_RoutingServ_OffsetTooLarge`, `WS_RoutingServ_ReconnectImpossible`, `WS_RoutingServ_RegistrationAlreadyClosed`, `WS_RoutingServ_RegistrationAlreadyOpen`, `WS_RoutingServ_RegistrationClosed`, `WS_RoutingServ_ServerFull`, `WS_RoutingServ_SpectatorsNotAllowed`, `WS_RoutingServ_SubscriptionAlreadyExists`, `WS_RoutingServ_SubscriptionDoesNotExist`

`WS_*` enum values defined in tree but absent from exe: 35.

`WS_AuthServ_KeyExpired`, `WS_AuthServ_KeyNotActive`, `WS_AuthServ_SetCUDError`, `WS_CommServ_StatusCommon_BannedAddress`, `WS_DBProxyServ_AccountCreateDisabled`, `WS_DBProxyServ_AgeNotOldEnough`, `WS_DBProxyServ_DBError`, `WS_DBProxyServ_DataAlreadyExists`, `WS_DBProxyServ_EMailPasswordDuplication`, `WS_DBProxyServ_GameSpecificErrorEnd`, `WS_DBProxyServ_GameSpecificErrorStart`, `WS_DBProxyServ_GameSpecificWarriorNameDoesNotExist`, `WS_DBProxyServ_InvalidCDKey`, `WS_DBProxyServ_InvalidPassword`, `WS_DBProxyServ_InvalidUserName`, `WS_DBProxyServ_ItemHasDependencies`, `WS_DBProxyServ_KeyInUse`, `WS_DBProxyServ_KeyNotUsed`, `WS_DBProxyServ_NoData`, `WS_DBProxyServ_NoModule`, `WS_DBProxyServ_OutOfDate`, `WS_DBProxyServ_OutOfDateNoUpdate`, `WS_DBProxyServ_PerCDKeyCheckRequired`, `WS_DBProxyServ_UserDoesNotExist`, `WS_DBProxyServ_UserExists`, `WS_DBProxyServ_ValidNotLatest`, `WS_DirServ_MultiGetFailedAllRequests`, `WS_DirServ_MultiGetPartialFailure`, `WS_EPIPE`, `WS_RoutingServG2_GroupNameExceedsMaximum`, `WS_RoutingServG2_JoinCanceled`, `WS_RoutingServG2_Throttled`, `WS_RoutingServG2_TooManyMemberships`, `WS_RoutingServG2_TooManyRecipients`, `WS_WSAHOST_NOT_FOUND`

