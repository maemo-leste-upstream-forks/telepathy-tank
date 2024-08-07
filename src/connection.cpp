/*
    This file is part of the telepathy-tank connection manager.
    Copyright (C) 2018 Alexandr Akulich <akulichalexander@gmail.com>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "connection.hpp"
#include "messageschannel.hpp"
#include "requestdetails.hpp"

#include <TelepathyQt/Constants>
#include <TelepathyQt/BaseChannel>

#include <QDebug>

#include <QStandardPaths>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QTimer>

// Quotient
#include <connection.h>
#include <room.h>
#include <settings.h>
#include <csapi/joining.h>
#include <csapi/leaving.h>
#include <events/simplestateevents.h>
#include <events/roomcanonicalaliasevent.h>
#include <user.h>

#define Q_MATRIX_CLIENT_MAJOR_VERSION 0
#define Q_MATRIX_CLIENT_MINOR_VERSION 1
#define Q_MATRIX_CLIENT_MICRO_VERSION 0
#define Q_MATRIX_CLIENT_VERSION ((Q_MATRIX_CLIENT_MAJOR_VERSION<<16)|(Q_MATRIX_CLIENT_MINOR_VERSION<<8)|(Q_MATRIX_CLIENT_MICRO_VERSION))
#define Q_MATRIX_CLIENT_VERSION_CHECK(major, minor, patch) ((major<<16)|(minor<<8)|(patch))

static const QString secretsDirPath = QLatin1String("/secrets/");
static const QString c_saslMechanismTelepathyPassword = QLatin1String("X-TELEPATHY-PASSWORD");
static const int c_sessionDataFormat = 1;

Tp::AvatarSpec MatrixConnection::getAvatarSpec()
{
    static const auto spec = Tp::AvatarSpec({ QStringLiteral("image/png") },
                                            0 /* minHeight */,
                                            512 /* maxHeight */,
                                            256 /* recommendedHeight */,
                                            0 /* minWidth */,
                                            512 /* maxWidth */,
                                            256 /* recommendedWidth */,
                                            1024 * 1024 /* maxBytes */);
    return spec;
}

Tp::SimpleStatusSpecMap MatrixConnection::getSimpleStatusSpecMap()
{
    static const Tp::SimpleStatusSpecMap map = []() {
        // https://github.com/matrix-org/matrix-doc/blob/master/specification/modules/presence.rst#presence
        Tp::SimpleStatusSpec onlineStatus;
        onlineStatus.type = Tp::ConnectionPresenceTypeAvailable;
        onlineStatus.maySetOnSelf = true;
        onlineStatus.canHaveMessage = false;

        Tp::SimpleStatusSpec unavailableStatus;
        unavailableStatus.type = Tp::ConnectionPresenceTypeAway;
        unavailableStatus.maySetOnSelf = true;
        unavailableStatus.canHaveMessage = false;

        Tp::SimpleStatusSpec offlineStatus;
        offlineStatus.type = Tp::ConnectionPresenceTypeOffline;
        offlineStatus.maySetOnSelf = true;
        offlineStatus.canHaveMessage = false;

        return QMap<QString,Tp::SimpleStatusSpec>({
                                                      { QLatin1String("available"), onlineStatus },
                                                      { QLatin1String("unavailable"), unavailableStatus },
                                                      { QLatin1String("offline"), offlineStatus },
                                                  });
    }();
    return map;
}

Tp::SimplePresence MatrixConnection::mkSimplePresence(MatrixPresence presence, const QString &statusMessage)
{
    switch (presence) {
    case MatrixPresence::Online:
        return { Tp::ConnectionPresenceTypeAvailable, QLatin1String("available"), statusMessage };
    case MatrixPresence::Unavailable:
        return { Tp::ConnectionPresenceTypeAway, QLatin1String("unavailable"), statusMessage };
    case MatrixPresence::Offline:
        return { Tp::ConnectionPresenceTypeOffline, QLatin1String("offline"), statusMessage };
    }
    return { Tp::ConnectionPresenceTypeError, QLatin1String("error"), statusMessage };
}

Tp::RequestableChannelClassSpecList MatrixConnection::getRequestableChannelList()
{
    Tp::RequestableChannelClassSpecList result;
    Tp::RequestableChannelClass personalChat;
    personalChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType")] = TP_QT_IFACE_CHANNEL_TYPE_TEXT;
    personalChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType")]  = Tp::HandleTypeContact;
    personalChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"));
    personalChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID"));
    result.append(personalChat);

    Tp::RequestableChannelClass groupChat;
    groupChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType")] = TP_QT_IFACE_CHANNEL_TYPE_TEXT;
    groupChat.fixedProperties[TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType")]  = Tp::HandleTypeRoom;
    groupChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"));
    groupChat.allowedProperties.append(TP_QT_IFACE_CHANNEL + QLatin1String(".TargetID"));
    result.append(groupChat);

    return result;
}

MatrixConnection::MatrixConnection(const QDBusConnection &dbusConnection, const QString &cmName,
                                   const QString &protocolName, const QVariantMap &parameters)
    : Tp::BaseConnection(dbusConnection, cmName, protocolName, parameters)
{
    qDebug() << Q_FUNC_INFO << parameters;

    /* Connection.Interface.Contacts */
    contactsIface = Tp::BaseConnectionContactsInterface::create();
    contactsIface->setGetContactAttributesCallback(Tp::memFun(this, &MatrixConnection::getContactAttributes));
    contactsIface->setContactAttributeInterfaces({
                                                     TP_QT_IFACE_CONNECTION,
                                                     TP_QT_IFACE_CONNECTION_INTERFACE_ALIASING,
                                                     TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_GROUPS,
                                                     TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST,
                                                     TP_QT_IFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE,
                                                     TP_QT_IFACE_CONNECTION_INTERFACE_AVATARS,
                                                 });
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(contactsIface));

    /* Connection.Interface.Aliasing */
    m_aliasingIface = Tp::BaseConnectionAliasingInterface::create();
    m_aliasingIface->setGetAliasesCallback(Tp::memFun(this, &MatrixConnection::getAliases));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(m_aliasingIface));

    /* Connection.Interface.SimplePresence */
    m_simplePresenceIface = Tp::BaseConnectionSimplePresenceInterface::create();
    m_simplePresenceIface->setStatuses(getSimpleStatusSpecMap());
    m_simplePresenceIface->setSetPresenceCallback(Tp::memFun(this, &MatrixConnection::setPresence));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(m_simplePresenceIface));

    /* Connection.Interface.ContactList */
    m_contactListIface = Tp::BaseConnectionContactListInterface::create();
    m_contactListIface->setContactListPersists(true);
    m_contactListIface->setCanChangeContactList(true);
    m_contactListIface->setDownloadAtConnection(true);
    m_contactListIface->setGetContactListAttributesCallback(Tp::memFun(this, &MatrixConnection::getContactListAttributes));
    m_contactListIface->setRequestSubscriptionCallback(Tp::memFun(this, &MatrixConnection::requestSubscription));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(m_contactListIface));

    /* Connection.Interface.ContactGroups */
    Tp::BaseConnectionContactGroupsInterfacePtr contactGroupsIface = Tp::BaseConnectionContactGroupsInterface::create();
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(contactGroupsIface));

    /* Connection.Interface.Requests */
    m_requestsIface = Tp::BaseConnectionRequestsInterface::create(this);
    m_requestsIface->requestableChannelClasses = getRequestableChannelList().bareClasses();

    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(m_requestsIface));

    setConnectCallback(Tp::memFun(this, &MatrixConnection::doConnect));
    setInspectHandlesCallback(Tp::memFun(this, &MatrixConnection::inspectHandles));
    setCreateChannelCallback(Tp::memFun(this, &MatrixConnection::createChannelCB));
    setRequestHandlesCallback(Tp::memFun(this, &MatrixConnection::requestHandles));

    m_user = parameters.value(QLatin1String("user")).toString();
    m_password = parameters.value(QLatin1String("password")).toString();
    m_deviceId = parameters.value(QLatin1String("device"), QStringLiteral("HomePC")).toString();
    m_server = parameters.value(QLatin1String("server"), QStringLiteral("https://matrix.org")).toString();

    /* Connection.Interface.Avatars */
    m_avatarsIface = Tp::BaseConnectionAvatarsInterface::create();
    m_avatarsIface->setAvatarDetails(MatrixConnection::getAvatarSpec());
    m_avatarsIface->setGetKnownAvatarTokensCallback(Tp::memFun(this, &MatrixConnection::getKnownAvatarTokens));
    m_avatarsIface->setRequestAvatarsCallback(Tp::memFun(this, &MatrixConnection::requestAvatars));
    plugInterface(Tp::AbstractConnectionInterfacePtr::dynamicCast(m_avatarsIface));

    connect(this, &MatrixConnection::disconnected, this, &MatrixConnection::doDisconnect);
}

MatrixConnection::~MatrixConnection()
{
}

void MatrixConnection::doConnect(Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << m_user << m_password << m_deviceId;
    setStatus(Tp::ConnectionStatusConnecting, Tp::ConnectionStatusReasonRequested);

    m_connection = new Quotient::Connection(QUrl(m_server));

    // requires -DQuotient_ENABLE_E2EE=ON for libquotient
    m_connection->enableEncryption(true);
    m_connection->enableDirectChatEncryption(true);

    connect(m_connection, &Quotient::Connection::connected, this, &MatrixConnection::onConnected);
    connect(m_connection, &Quotient::Connection::syncDone, this, &MatrixConnection::onSyncDone);
    connect(m_connection, &Quotient::Connection::loginError, [](const QString &error) {
        qDebug() << "Login error: " << error;
    });
//    connect(m_connection, &Quotient::Connection::networkError, [](size_t nextAttempt, int inMilliseconds) {
//        qDebug() << "networkError: " << nextAttempt << "millis" << inMilliseconds;
//    });
    connect(m_connection, &Quotient::Connection::resolveError, [](const QString &error) {
        qDebug() << "Resolve error: " << error;
    });
    connect(m_connection, &Quotient::Connection::newRoom, this, &MatrixConnection::processNewRoom);

    // disable loading session for now, `connectWithToken` has issues
    // if (loadSessionData()) {
    //     qDebug() << Q_FUNC_INFO << "connectWithToken" << m_user << m_accessToken << m_deviceId;
    //     m_connection->loginWithToken(QString::fromLatin1(m_accessToken), m_deviceId);
    // } else {
    m_connection->loginWithPassword(m_user, m_password, m_deviceId);
    // }
}

void MatrixConnection::doDisconnect()
{
    if (!m_connection) {
        return;
    }
    m_connection->stopSync();
    setStatus(Tp::ConnectionStatusDisconnected, Tp::ConnectionStatusReasonRequested);
}

QStringList MatrixConnection::inspectHandles(uint handleType, const Tp::UIntList &handles, Tp::DBusError *error)
{
    QStringList *knownIds = nullptr;
    switch (handleType) {
    case Tp::HandleTypeContact:
        knownIds = &m_contactIds;
        break;
    case Tp::HandleTypeRoom:
        knownIds = &m_roomIds;
        break;
    default:
        error->set(TP_QT_ERROR_INVALID_ARGUMENT, QStringLiteral("Unsupported handle type"));
        return {};
    }
    QStringList result;
    result.reserve(handles.count());
    for (const uint handle : handles) {
        if ((handle == 0) || (handle > static_cast<uint>(knownIds->count()))) {
            if (error) {
                error->set(TP_QT_ERROR_INVALID_HANDLE, QStringLiteral("Invalid handle"));
                return {};
            }
        }
        result.append(knownIds->at(handle - 1));
    }
    return result;
}

Tp::UIntList MatrixConnection::requestHandles(uint handleType, const QStringList &identifiers, Tp::DBusError *error)
{
    QStringList *knownIds = nullptr;
    switch (handleType) {
    case Tp::HandleTypeContact:
        knownIds = &m_contactIds;
        break;
    case Tp::HandleTypeRoom:
        knownIds = &m_roomIds;
        break;
    default:
        error->set(TP_QT_ERROR_INVALID_ARGUMENT, QStringLiteral("Unsupported handle type"));
        return {};
    }
    Tp::UIntList result;
    result.reserve(identifiers.count());
    for (const QString &id : identifiers) {
        int handle = knownIds->indexOf(id) + 1;
        if (handle == 0) {
            if (error) {
                error->set(TP_QT_ERROR_INVALID_ARGUMENT, QStringLiteral("Unknown identifier"));
                return {};
            }
        }
        result.append(handle);
    }
    return result;
}

Quotient::JoinRoomJob* MatrixConnection::joinRoomSync(QString alias)
{
    qDebug() << "joinRoomSync" << alias;
    auto *job = m_connection->joinRoom(alias, {});  // @TODO: support serverList
    QEventLoop loop;
    connect(job, &Quotient::JoinRoomJob::finished, &loop, &QEventLoop::quit);
    loop.exec();

    qDebug() << "joinRoomSync code" << job->status().code << "msg" << job->status().message;
    return job;
}

Quotient::LeaveRoomJob* MatrixConnection::leaveRoomSync(Quotient::Room *room)
{
    qDebug() << "leaveRoomSync" << room->id();
    Quotient::LeaveRoomJob *job = room->leaveRoom();
    QEventLoop loop;
    connect(job, &Quotient::LeaveRoomJob::finished, &loop, &QEventLoop::quit);
    loop.exec();

    qDebug() << "leaveRoomSync code" << job->status().code << "msg" << job->status().message;
    return job;
}

Quotient::CreateRoomJob* MatrixConnection::createRoomSync(QString alias)
{
    Quotient::Connection::RoomVisibility visibility = Quotient::Connection::RoomVisibility::PublishRoom;
    QString topic = "";
    QStringList invites = {};

    // ':' is not permitted in the room alias name.
    // This expects a local part - 'test', not '#test:utwente.io'
    if(alias.contains(":")) {
        alias = alias.split(":").at(0);
    }

    qDebug() << "createRoomSync" << alias;
    auto *job = m_connection->createRoom(visibility, alias, alias.replace("#", ""), topic, invites);
    QEventLoop loop;
    connect(job, &Quotient::CreateRoomJob::finished, &loop, &QEventLoop::quit);
    loop.exec();

    qDebug() << "createRoomSync code" << job->status().code << "msg" << job->status().message;
    return job;
}

Tp::BaseChannelPtr MatrixConnection::bogusChannel(Tp::BaseChannelPtr baseChannel, Tp::DBusError *error, QString err_msg) 
{
    error->set(TP_QT_ERROR_INVALID_ARGUMENT, err_msg);
    qWarning() << err_msg;
    return baseChannel;
}

Tp::BaseChannelPtr MatrixConnection::createChannelCB(const QVariantMap &request, Tp::DBusError *error)
{
    // TargetID (remote_uid) can be of the following 2 formats:
    // 1. Matrix Room Alias #test:utwente.io
    // 2. Matrix Room ID !tRtcUJTDuqHvuIxdyd:utwente.io
    //
    // Telepathy clients should prefer Room ID, except, of course, for the first
    // join where an user provided a room by alias - as Room ID is mostly an
    // underlying matrix protocol concept.
    //
    // Room name is eventually derived from the Quotient::Room* object (via canonicalName())
    // which is handled by the messageschannel, in the constructor (Tp::BaseChannelRoomInterface).
    //
    // During the join/create flow, we return a bogus channel, as after Quotient::Room* creation
    // it wont have its full state yet. By listening to Quotient::Room* events we will 
    // trigger this function a second time when it is ready (MatrixConnection::onAboutToAddNewMessages())

    const RequestDetails details = request;

    if (details.channelType() == TP_QT_IFACE_CHANNEL_TYPE_ROOM_LIST)
        return createRoomListChannel();

    const Tp::HandleType targetHandleType = details.targetHandleType();
    const uint targetHandle = details.getTargetHandle(this);
    QString targetID = details.getTargetIdentifier(this);
    const QString init = details.getInitiatorID(this);

    qDebug() << "============= createChannelCB";
    qDebug() << "targetID" << targetID;
    // qDebug() << "targetHandle" << targetHandle;
    // qDebug() << "targetHandleType is_contact" << (targetHandleType == Tp::HandleTypeContact);
    qDebug() << "targetHandleType is_room" << (targetHandleType == Tp::HandleTypeRoom);
    // qDebug() << "initiatorID" << init;

    Tp::BaseChannelPtr baseChannel = Tp::BaseChannel::create(this, details.channelType(), targetHandleType, targetHandle);
    baseChannel->setRequested(details.isRequested());

    switch (targetHandleType) {
    case Tp::HandleTypeContact:
    case Tp::HandleTypeRoom:
        break;
    case Tp::HandleTypeNone:
        return bogusChannel(baseChannel, error, "Target handle type is not present in the request details");
    default:
        return bogusChannel(baseChannel, error, "Unknown target handle type");
    }

    if (targetID.isEmpty())
        return bogusChannel(baseChannel, error, "Unknown target identifier");

    // Quotient::Room *Leaves the chat room.
    Quotient::Room *targetRoom = nullptr;
    QString room_id = targetID;
    QString room_name = "";
    QString err_msg = "";
    if(targetID.startsWith("#") || targetID.startsWith("@"))
        room_name = targetID;

    if (targetHandleType == Tp::HandleTypeContact) {
        DirectContact contact = getDirectContact(targetHandle);
        if (!contact.isValid())
            return bogusChannel(baseChannel, error, "Requested single chat does not exist yet (and DirectChat creation is not supported yet)");

        targetRoom = contact.room;
    } else {
        targetRoom = getRoom(targetHandle);

        // 1. try joining a room if we are not aware of it yet
        // 2. try creating a room if joining did not work
        //
        // Note: when leaving a channel, and we were the last participant, the room
        // will get permanently deleted server-side, and there is no way to rejoin or
        // recreate, regardless of using Room Alias or Room ID.
        if(!targetRoom) {
            qDebug() << "join/create flow";
            auto *joinJob = this->joinRoomSync(targetID);
            auto join_status_code = joinJob->status().code;

            if(join_status_code == 0) {
                room_id = joinJob->roomId();
            } else if(join_status_code == 101) {  // e.g "Failed to make_join via any server"
                joinJob->deleteLater();
                return bogusChannel(baseChannel, error, QString("join: Room does not exist anymore (%1) message (%2)").arg(targetID, joinJob->status().message));
            } else if(join_status_code == 105) { // Room not found, and possibly open for creation
                if(joinJob->status().message.contains("No known servers")) {
                    joinJob->deleteLater();
                    return bogusChannel(baseChannel, error, QString("join: Room alias has previously already been registered and cannot be re-used (%1) message (%2)").arg(targetID, joinJob->status().message));
                }

                auto *createJob = this->createRoomSync(targetID);
                if(createJob->status().code == 0) {
                    qDebug() << QString("channel %1 created").arg(targetID);
                    room_id = createJob->roomId();
                } else {
                    joinJob->deleteLater();
                    createJob->deleteLater();
                    return bogusChannel(baseChannel, error, QString("error createRoomJob, returning baseChannel"));
                }
            } else {
                joinJob->deleteLater();
                return bogusChannel(baseChannel, error, QString("join: unknown error for (%1) message %2, returning baseChannel").arg(targetID, joinJob->status().message));
            }

            if(!m_roomIds.contains(room_id)) {
                joinJob->deleteLater();
                return bogusChannel(baseChannel, error, "underlying Quotient::Room* not found");
            }

            qDebug() << "JoinRoom / createRoom success, returning bogus connection";
            return baseChannel;
        }
    }

    if (details.channelType() != TP_QT_IFACE_CHANNEL_TYPE_TEXT)
        return bogusChannel(baseChannel, error, "channelType was not TP_QT_IFACE_CHANNEL_TYPE_TEXT");

    if (!targetRoom)
        return bogusChannel(baseChannel, error, "targetRoom was nullptr");

    if(targetID.startsWith("!"))
        baseChannel->setTargetID(targetID);
    else if(room_id.startsWith("!"))
        baseChannel->setTargetID(room_id);

    if(room_name.isEmpty()) {
        room_name = targetRoom->canonicalAlias();
        if(room_name.isEmpty()) {
            return bogusChannel(baseChannel, error, "impossible situation, debug me");
        }
    }

    MatrixMessagesChannelPtr messagesChannel = MatrixMessagesChannel::create(this, room_name, targetRoom, baseChannel.data());
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(messagesChannel));
    connect(messagesChannel.data(), &MatrixMessagesChannel::destroyed, this, &MatrixConnection::onTextChannelClosed);

    connect(messagesChannel.data(), &MatrixMessagesChannel::channelLeft, [=]{
        this->onChannelLeft(targetRoom);
        baseChannel->close();

        // @TODO: on channel leave, `messagesChannel` stays alive - destructor not called. 
        // For now, lets assume Tp handles ownership.
    });

    // baseChannel closed slot
    connect(baseChannel.data(), &Tp::BaseChannel::closed, [this, targetRoom, messagesChannel, baseChannel] {
        qDebug() << "Tp::BaseChannel::closed";
    });

    return baseChannel;
}

void MatrixConnection::onTextChannelClosed()
{
    qDebug() << Q_FUNC_INFO;
    // @TODO: how2delete?
    // MatrixMessagesChannel *channel = static_cast<MatrixMessagesChannel*>(sender());
}

bool MatrixConnection::requestLeave(Quotient::Room *room)
{
    auto *job = this->leaveRoomSync(room);
    return job->status().code == 0;
}

void MatrixConnection::onChannelLeft(Quotient::Room *room)
{
    qDebug() << "leaving channel" << room->id();
    if(!this->requestLeave(room)) {
        qWarning() << "request leave failed";
    } else {
        qDebug() << "removing from m_roomIds";
        m_roomIds.removeAll(room->id());
    }
}

Tp::BaseChannelPtr MatrixConnection::createRoomListChannel()
{
    return Tp::BaseChannelPtr();
}

Tp::ContactAttributesMap MatrixConnection::getContactListAttributes(const QStringList &interfaces,
                                                                    bool hold, Tp::DBusError *error)
{
    Q_UNUSED(hold)
    return getContactAttributes(m_directContacts.keys(), interfaces, error);
}

Tp::ContactAttributesMap MatrixConnection::getContactAttributes(const Tp::UIntList &handles,
                                                                const QStringList &interfaces,
                                                                Tp::DBusError *error)
{
    // qDebug() << "getContactAttributes()";
    // qDebug() << Q_FUNC_INFO << handles << interfaces;
    Tp::ContactAttributesMap contactAttributes;

    for (auto handle : handles) {
        Quotient::User *user = getUser(handle);
        if (!user) {
            qWarning() << Q_FUNC_INFO << "No user for handle" << handle;
            continue;
        }
        contactAttributes[handle] = {};
        QVariantMap &attributes = contactAttributes[handle];

        // Attributes for all kind of contacts
        const QString id = handle == selfHandle() ? selfID() : user->id();
        attributes[TP_QT_IFACE_CONNECTION + QLatin1String("/contact-id")] = id;
        if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_AVATARS)) {
            attributes[TP_QT_IFACE_CONNECTION_INTERFACE_AVATARS + QLatin1String("/token")]
                    = QVariant::fromValue(user->avatarUrl().toString());
        }
        if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE)) {
            attributes[TP_QT_IFACE_CONNECTION_INTERFACE_SIMPLE_PRESENCE + QLatin1String("/presence")]
                    = QVariant::fromValue(mkSimplePresence(MatrixPresence::Online));
        }
        if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_ALIASING)) {
            attributes[TP_QT_IFACE_CONNECTION_INTERFACE_ALIASING + QLatin1String("/alias")]
                    = QVariant::fromValue(getContactAlias(handle));
        }
        if (handle == selfHandle()) {
            continue;
        }

        // Attributes not applicable for the self contact
        if (interfaces.contains(TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST)) {
            const Tp::SubscriptionState state = m_directContacts.contains(handle) ? Tp::SubscriptionStateYes : Tp::SubscriptionStateNo;
            attributes[TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST + QLatin1String("/subscribe")] = state;
            attributes[TP_QT_IFACE_CONNECTION_INTERFACE_CONTACT_LIST + QLatin1String("/publish")] = state;
        }
        // Attributes are taken by reference, no need to assign
    }
    // qDebug() << contactAttributes;
    // qDebug() << contactAttributes.count();
    return contactAttributes;
}

void MatrixConnection::requestSubscription(const Tp::UIntList &handles, const QString &message, Tp::DBusError *error)
{
    qDebug() << "requestSubscription()";
}

Tp::AliasMap MatrixConnection::getAliases(const Tp::UIntList &contacts, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << contacts;
    Tp::AliasMap aliases;
    for (uint handle : contacts) {
        aliases[handle] = getContactAlias(handle);
    }
    return aliases;
}

QString MatrixConnection::getContactAlias(uint handle) const
{
    qDebug() << "getContactAlias()";
    const Quotient::User *user = getUser(handle);
    if (!user) {
        return QString();
    }
    return user->displayname();
}

Tp::SimplePresence MatrixConnection::getPresence(uint handle)
{
    return {};
}

uint MatrixConnection::setPresence(const QString &status, const QString &message, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << status << "ret" << selfHandle();
    const Tp::SimpleStatusSpec spec = getSimpleStatusSpecMap().value(status);
    if (!spec.maySetOnSelf) {
        error->set(TP_QT_ERROR_INVALID_ARGUMENT, QStringLiteral("The requested presence can not be set on self contact"));
        return 0;
    }
    Tp::SimplePresence presence;
    presence.type = spec.type;
    presence.status = status;
    presence.statusMessage = message;
    m_simplePresenceIface->setPresences(Tp::SimpleContactPresences({{selfHandle(), presence}}));
    return selfHandle();
}

// 1. handle 1:1 room join - create a channel
// 2. handle room leave from external clients, close the channel
void MatrixConnection::handleMatrixMemberEvent(Quotient::Room* room, QJsonObject blob)
{
    qDebug() << "handleMatrixMemberEvent()";

    MatrixMessagesChannelPtr textChannel;
    auto obj_content = blob["content"].toObject();
    auto obj_sender = blob["sender"].toString();
    if(!obj_content.contains("membership"))
        return;

    auto membership = obj_content["membership"].toString();
    if(membership == "join" && obj_sender != selfID() && room->isDirectChat()) {  // 1:1 room, create channel
        textChannel = getMatrixMessagesChannelPtr(room);
        return;
    }

    // detect if we left a room
    if(membership != "leave" || obj_sender != selfID())
        return;

    uint handleType = room->isDirectChat() ? Tp::HandleTypeContact : Tp::HandleTypeRoom;
    uint handle = room->isDirectChat() ? getDirectContactHandle(room) : getRoomHandle(room);
    if (!handle) {
        qWarning() << Q_FUNC_INFO << "Unknown room" << room->id();
        return;
    }

    // get existing channel, close
    Tp::DBusError error;
    qDebug() << "get getExistingChannel for removal";
    Tp::BaseChannelPtr channel = getExistingChannel(
        QVariantMap({
            { TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType"), handleType },
            { TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"), handle },
            { TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType"), TP_QT_IFACE_CHANNEL_TYPE_TEXT },
        }), &error);

    if (error.isValid()) {
        qWarning() << "getExistingChannel failed:" << error.name() << " " << error.message();
        return;
    }

    auto _channel = channel->interface(TP_QT_IFACE_CHANNEL_TYPE_TEXT);
    if(!_channel) {
        qDebug() << "could not close non-existent channel";
        return;
    }

    textChannel = MatrixMessagesChannelPtr::dynamicCast(_channel);
    if(textChannel && channel) {
        qDebug() << "closing channel";
        channel->close();  // @TODO: what about `m_groupIface->setRemoveMembersCallback();` ?
    } else {
        qWarning() << "cannot call channel->close() when either textChannel or channel is nullptr";
    }
}

// Quotient::Room event handler, e.g: channel creation, messages, name events
//
// responsible for the creation of a Telepathy channel (createChannelCB())
// when the Quotient::Room is "ready". Upon creation of a new Quotient::Room*
// instance, it may not have all of its state yet, this includes the canonical
// room alias, and we wait for it before channel creation. in addition, we cache
// incoming messages while the room is not in a ready state yet, as messages
// may come in before we have a room alias.
void MatrixConnection::onAboutToAddNewMessages(Quotient::RoomEventsRange events)
{
    MatrixMessagesChannelPtr textChannel;
    Quotient::RoomMessageEvent *message;

    auto _sender = sender();
    Quotient::Room *room = qobject_cast<Quotient::Room *>(_sender);
    if(!room) {
        qWarning() << "room was nullptr";
        return;
    }

    for (auto &event : events) {
        Quotient::RoomEvent* _event = event.get();
        QString event_type = event->originalJsonObject()["type"].toString();
        qDebug() << "event_type" << event_type;

        // detect room leave from external clients
        if(event_type == "m.room.member") {
            // qDebug() << event->originalJsonObject();
            handleMatrixMemberEvent(room, event->originalJsonObject());
            continue;
        }

        // skip messages when there is no canonicalAlias yet
        // except for 1:1 chats, which do not have a canonicalAlias
        if(!room->isDirectChat()) {
            if(room->canonicalAlias().isEmpty() && event_type == "m.room.message") {
                message = dynamic_cast<Quotient::RoomMessageEvent *>(_event);
                m_messageQueue[room->id()] << new RoomMessageEvent(message);
                continue;
            } else if(room->canonicalAlias().isEmpty()) {
                continue;
            }
        }

        if(QStringList({"m.room.member", "m.room.canonical_alias", "m.room.message"}).contains(event_type)) {
            textChannel = getMatrixMessagesChannelPtr(room);  // createChannelCb()
            if (!textChannel)
                continue;

            // any cached messages?
            if(m_messageQueue.contains(room->id())) {
                for(RoomMessageEvent* cachedMessageEvent: m_messageQueue[room->id()])
                    textChannel->processMessageEvent(cachedMessageEvent);
                m_messageQueue.clear();
            }

            // handle incoming message
            if(event_type == "m.room.message") {
                Quotient::RoomMessageEvent *message = dynamic_cast<Quotient::RoomMessageEvent *>(_event);
                textChannel->processMessageEvent(message);
            }
        }
    }
}

void MatrixConnection::onConnected()
{
    m_userId = m_connection->userId();

    uint selfId = ensureContactHandle(m_userId);
    if (selfId != 1) {
        qWarning() << "Self ID seems to be set too late";
    }
    setSelfContact(selfId, m_userId);

    setStatus(Tp::ConnectionStatusConnected, Tp::ConnectionStatusReasonRequested);
    m_contactListIface->setContactListState(Tp::ContactListStateWaiting);

    qDebug() << Q_FUNC_INFO;
    saveSessionData();

    m_connection->syncLoop();
}

void MatrixConnection::onSyncDone()
{
    qDebug() << Q_FUNC_INFO;

    // any to join?
    for (Quotient::Room *room : m_connection->rooms(Quotient::JoinState::Join)) {
        processNewRoom(room);
    }

    // any invites?
    for (Quotient::Room *room : m_connection->rooms(Quotient::JoinState::Invite)) {
        qDebug() << "invited to a room, auto-joining";
        processNewRoom(room);
        this->joinRoomSync(room->id());
    }

    m_contactListIface->setContactListState(Tp::ContactListStateSuccess);
}

void MatrixConnection::onUserAvatarChanged(Quotient::User *user)
{
    QByteArray outData;
    QBuffer output(&outData);
    const QImage ava = user->avatar(64, 64);
    qDebug() << Q_FUNC_INFO << ava.isNull();
    if (ava.isNull()) {
        return;
    }
    ava.save(&output, "png");
    m_avatarsIface->avatarRetrieved(ensureHandle(user), user->avatarUrl().toString(), outData, QStringLiteral("image/png"));
    qDebug() << Q_FUNC_INFO << "retrieved";
}

bool MatrixConnection::loadSessionData()
{
    qDebug() << Q_FUNC_INFO << QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + secretsDirPath + m_user;
    QFile secretFile(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + secretsDirPath + m_user);
    if (!secretFile.open(QIODevice::ReadOnly)) {
        qDebug() << Q_FUNC_INFO << "Unable to open file" << "for account" << m_user;
        return false;
    }
    const QByteArray data = secretFile.readAll();
    qDebug() << Q_FUNC_INFO << m_user << "(" << data.size() << "bytes)";
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    const int format = doc.object().value("format").toInt();
    if (format > c_sessionDataFormat) {
        qWarning() << Q_FUNC_INFO << "Unsupported file format" << format;
        return false;
    }

    QJsonObject session = doc.object().value("session").toObject();
    m_accessToken = QByteArray::fromHex(session.value(QLatin1String("accessToken")).toVariant().toByteArray());
    m_userId = session.value(QLatin1String("userId")).toString();
    m_homeServer = session.value(QLatin1String("homeServer")).toString();
    m_deviceId = session.value(QLatin1String("deviceId")).toString();

    return !m_accessToken.isEmpty();
}

bool MatrixConnection::saveSessionData() const
{
    if (!m_connection) {
        return false;
    }

    QJsonObject sessionObject;
    sessionObject.insert(QLatin1String("accessToken"), QString::fromLatin1(m_connection->accessToken().toHex()));
    sessionObject.insert(QLatin1String("userId"), m_connection->userId());
    sessionObject.insert(QLatin1String("homeServer"), m_connection->homeserver().toString());
    sessionObject.insert(QLatin1String("deviceId"), m_connection->deviceId());

    QJsonObject rootObject;
    rootObject.insert("session", sessionObject);
    rootObject.insert("format", c_sessionDataFormat);
    QJsonDocument doc(rootObject);

    const QByteArray data = doc.toJson(QJsonDocument::Indented);

    QDir dir;
    dir.mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + secretsDirPath);
    QFile secretFile(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + secretsDirPath + m_user);
    if (!secretFile.open(QIODevice::WriteOnly)) {
        qWarning() << Q_FUNC_INFO << "Unable to save the session data to file" << "for account" << m_user;
        return false;
    }

    qDebug() << Q_FUNC_INFO << m_user << "(" << data.size() << "bytes)";
    return secretFile.write(data) == data.size();
}

void MatrixConnection::processNewRoom(Quotient::Room *room)
{
    if (room->isDirectChat()) {
        // Single user room (1:1)
        for (Quotient::User *user : room->users()) {
            if (user == room->localUser()) {
                continue;
            }

            ensureDirectContact(user, room);
            getMatrixMessagesChannelPtr(room);
        }
    } else {
        // group room
        ensureHandle(room);
    }

    connect(room, &Quotient::Room::aboutToAddNewMessages,
            this, &MatrixConnection::onAboutToAddNewMessages,
            Qt::UniqueConnection);

    // name state comes later
    connect(room, &Quotient::Room::namesChanged, this, &MatrixConnection::onRoomNameChanged, Qt::UniqueConnection);
}

void MatrixConnection::onRoomNameChanged(Quotient::Room *room)
{
    qDebug() << QString("room alias for %1 is now %2").arg(room->id(), room->canonicalAlias());
}

uint MatrixConnection::ensureDirectContact(Quotient::User *user, Quotient::Room *room)
{
    const uint handle = ensureHandle(user);
    m_directContacts.insert(handle, DirectContact(user, room));
    qDebug() << "ensureDirectContact" << user->id() << "room" << room->id() << "directContact handle" << handle;
    return handle;
}

MatrixMessagesChannelPtr MatrixConnection::getMatrixMessagesChannelPtr(Quotient::Room *room)
{
    MatrixMessagesChannelPtr textChannel;
    uint handleType = room->isDirectChat() ? Tp::HandleTypeContact : Tp::HandleTypeRoom;
    uint handle = room->isDirectChat() ? getDirectContactHandle(room) : getRoomHandle(room);
    if (!handle) {
        qWarning() << "Unknown room" << room->id();
        return textChannel;
    }

    bool yoursChannel;
    Tp::DBusError error;
    Tp::BaseChannelPtr channel = ensureChannel(
                QVariantMap({
                                { TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandleType"), handleType },
                                { TP_QT_IFACE_CHANNEL + QLatin1String(".TargetHandle"), handle },
                                { TP_QT_IFACE_CHANNEL + QLatin1String(".ChannelType"), TP_QT_IFACE_CHANNEL_TYPE_TEXT },
                            }),
                yoursChannel, /* suppress handle */ false, &error);
    if (error.isValid()) {
        qWarning() << "ensureChannel failed:" << error.name() << " " << error.message();
        return textChannel;
    }

    textChannel = MatrixMessagesChannelPtr::dynamicCast(channel->interface(TP_QT_IFACE_CHANNEL_TYPE_TEXT));
    return textChannel;
}

void MatrixConnection::prefetchHistory(Quotient::Room *room)
{
    if (room->messageEvents().begin() == room->messageEvents().end())
        return;

    MatrixMessagesChannelPtr textChannel = getMatrixMessagesChannelPtr(room);
    if (!textChannel) {
        qDebug() << "Error, channel is not a TextChannel?";
        return;
    }
    textChannel->fetchHistory();
}

Quotient::User *MatrixConnection::getUser(uint handle) const
{
    if (handle == 0 || handle > static_cast<uint>(m_contactIds.count())) {
        qWarning() << Q_FUNC_INFO << "Invalid handle";
        return nullptr;
    }
    if (handle == selfHandle()) {
        return m_connection->user();
    }
    const QString id = m_contactIds.at(handle - 1);
    return m_connection->user(id);
}

Quotient::User *MatrixConnection::getUser(const QString &id) const
{
    if (id == selfID()) {
        return m_connection->user();
    }
    return m_connection->user(id);
}

DirectContact MatrixConnection::getDirectContact(uint contactHandle) const
{
    return m_directContacts.value(contactHandle);
}

Quotient::Room *MatrixConnection::getRoom(uint handle) const
{
    if (handle == 0 || handle > static_cast<uint>(m_roomIds.count())) {
        qWarning() << Q_FUNC_INFO << "Invalid handle";
        return nullptr;
    }
    const QString id = m_roomIds.at(handle - 1);
    return m_connection->room(id);
}

uint MatrixConnection::getContactHandle(Quotient::User *user)
{
    return m_contactIds.indexOf(user->id()) + 1;
}

uint MatrixConnection::getDirectContactHandle(Quotient::Room *room)
{
    for (uint handle : m_directContacts.keys()) {
        if (m_directContacts.value(handle).room == room) {
            return handle;
        }
    }
    return 0;
}

uint MatrixConnection::getRoomHandle(Quotient::Room *room)
{
    return m_roomIds.indexOf(room->id()) + 1;
}

uint MatrixConnection::ensureHandle(Quotient::User *user)
{
    uint index = getContactHandle(user);
    if (index != 0) {
        return index;
    }
    m_contactIds.append(user->id());
    return m_contactIds.count();
}

uint MatrixConnection::ensureHandle(Quotient::Room *room)
{
    uint index = getRoomHandle(room);
    if (index != 0) {
        return index;
    }

    m_roomIds.append(room->id());
    qDebug() << "ensureHandle()";
    qDebug() << "id()" << room->id();
    qDebug() << "name()" << room->name();
    qDebug() << "canonicalAlias()" << room->canonicalAlias();
    qDebug() << "members size" << room->memberNames().size();

    auto state = room->joinState();
    if(state == Quotient::JoinState::Join) {
        qDebug() << "state: Quotient::JoinState::Join";
    } else if(state == Quotient::JoinState::Leave) {
        qDebug() << "state: Quotient::JoinState::Leave";
    } else if(state == Quotient::JoinState::Invite) {
        qDebug() << "state: Quotient::JoinState::Invite";
    } else if(state == Quotient::JoinState::Knock) {
        qDebug() << "state: Quotient::JoinState::Knock";
    }

    qDebug() << "appending to m_roomIds" << room->id();
    return m_roomIds.count();
}

uint MatrixConnection::ensureContactHandle(const QString &identifier)
{
    int index = m_contactIds.indexOf(identifier);
    if (index < 0) {
        m_contactIds.append(identifier);
        return m_contactIds.count();
    }
    return index + 1;
}

void MatrixConnection::requestAvatars(const Tp::UIntList &handles, Tp::DBusError *error)
{
    requestAvatarsImpl(handles);
}

Tp::AvatarTokenMap MatrixConnection::getKnownAvatarTokens(const Tp::UIntList &handles, Tp::DBusError *error)
{
    qDebug() << Q_FUNC_INFO << handles;
    if (error->isValid()) {
        return {};
    }
    Tp::AvatarTokenMap result;
    for (uint handle : handles) {
        const Quotient::User *user = getUser(handle);
        if (user && user->avatarUrl().isValid()) {
            result.insert(handle, user->avatarUrl().toString());
        }
    }
    return result;
}

void MatrixConnection::requestAvatarsImpl(const Tp::UIntList &handles)
{
    qDebug() << Q_FUNC_INFO << handles;
    for (auto handle : handles) {
        Quotient::User *user = getUser(handle);
        if (!user) {
            continue;
        }

        // @TODO: disable the avatar stuff, for now
        //connect(user, &Quotient::User::avatarChanged, this, &MatrixConnection::onUserAvatarChanged);
        //onUserAvatarChanged(user);
    }
}
