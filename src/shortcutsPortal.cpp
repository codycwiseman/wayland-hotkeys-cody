/*
    OBS Wayland Hotkeys
    Copyright (C) 2025 Leia <leia@tutamail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "shortcutsPortal.h"

#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <obs.h>

#include <QCryptographicHash>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QWindow>

#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#include <private/qdesktopunixservices_p.h>
#else
#include <private/qgenericunixservices_p.h>
#endif
#include <private/qguiapplication_p.h>
#include <qpa/qplatformintegration.h>

using namespace Qt::Literals::StringLiterals;

static const QString freedesktopDest = u"org.freedesktop.portal.Desktop"_s;
static const QString freedesktopPath = u"/org/freedesktop/portal/desktop"_s;
static const QString globalShortcutsInterface = u"org.freedesktop.portal.GlobalShortcuts"_s;

ShortcutsPortal::ShortcutsPortal(QObject* parent)
    : QObject(parent)
{
    obs_frontend_add_event_callback(obsFrontendEvent, this);
}

void ShortcutsPortal::createSession()
{
    blog(LOG_INFO, "[ShortcutsPortal] Creating session request...");
    QDBusMessage createSessionCall = QDBusMessage::createMethodCall(
        freedesktopDest,
        freedesktopPath,
        globalShortcutsInterface,
        u"CreateSession"_s
    );

    QList<QVariant> createSessionArgs;

    QMap<QString, QVariant> sessionOptions;
    sessionOptions.insert(u"handle_token"_s, m_handleToken);
    sessionOptions.insert(u"session_handle_token"_s, m_sessionHandleToken);
    createSessionArgs.append(sessionOptions);
    createSessionCall.setArguments(createSessionArgs);

    QDBusMessage call = QDBusConnection::sessionBus().call(createSessionCall);
    if (call.type() != QDBusMessage::ReplyMessage) {
        auto errMsg = QMessageBox(m_parentWindow);
        errMsg.critical(m_parentWindow, u"Failed to create global shortcuts session"_s, call.errorMessage());
        blog(LOG_ERROR, "[ShortcutsPortal] Failed to create session: %s", call.errorMessage().toUtf8().constData());
    }

    this->m_responseHandle = call.arguments().first().value<QDBusObjectPath>();

    qDBusRegisterMetaType<QPair<QString, QVariantMap>>();
    qDBusRegisterMetaType<QList<QPair<QString, QVariantMap>>>();

    QDBusConnection::sessionBus().connect(
        freedesktopDest,
        m_responseHandle.path(),
        u"org.freedesktop.portal.Request"_s,
        u"Response"_s,
        this,
        SLOT(onCreateSessionResponse(unsigned int, QVariantMap))
    );
}

int ShortcutsPortal::getVersion()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        freedesktopDest,
        freedesktopPath,
        u"org.freedesktop.DBus.Properties"_s,
        u"Get"_s
    );

    message.setArguments({globalShortcutsInterface, u"version"_s});
    QDBusMessage reply = QDBusConnection::sessionBus().call(message);
    auto version = reply.arguments().first().value<QDBusVariant>().variant().toUInt();
    return version;
}

void ShortcutsPortal::createShortcut(
    const QString& name,
    const QString& description,
    const std::function<void(bool)>& callbackFunc
)
{
    m_shortcuts[name] = PortalShortcut{name, description, callbackFunc};
}

void ShortcutsPortal::createShortcuts()
{
    blog(LOG_INFO, "[ShortcutsPortal] Re-creating shortcuts list...");
    m_shortcuts.clear();

    // Collect valid source pointers to ensure safety
    QSet<void*> validSources;
    obs_enum_sources([](void* data, obs_source_t* source) {
        auto* set = static_cast<QSet<void*>*>(data);
        set->insert(static_cast<void*>(source));

        // Also add filters for this source
        obs_source_enum_filters(source, [](obs_source_t*, obs_source_t* filter, void* p) {
            auto* s = static_cast<QSet<void*>*>(p);
            s->insert(static_cast<void*>(filter));
        }, set);

        return true;
    }, &validSources);

    // Store a pointer to the valid sources set temporarily for the hotkey enumeration callback
    m_currentValidSources = &validSources;

    obs_enum_hotkeys(
        [](void* data, obs_hotkey_id id, obs_hotkey_t* binding) {
            auto t = static_cast<ShortcutsPortal*>(data);
            auto* validSources = static_cast<QSet<void*>*>(t->m_currentValidSources);

            const char* descStr = obs_hotkey_get_description(binding);
            QString description = descStr ? QString::fromUtf8(descStr) : QString();

            if (description.isEmpty()) {
                 const char* nameStr = obs_hotkey_get_name(binding);
                 description = nameStr ? QString::fromUtf8(nameStr) : "Unknown Hotkey";
            }

            QString namePrefix;
            obs_hotkey_registerer_type type = obs_hotkey_get_registerer_type(binding);
            void* registerer = obs_hotkey_get_registerer(binding);

            if (registerer) {
                const char* name = nullptr;
                if (type == OBS_HOTKEY_REGISTERER_SOURCE) {
                    // Only access the source if we verified it exists
                    if (validSources && validSources->contains(registerer)) {
                        name = obs_source_get_name(static_cast<obs_source_t*>(registerer));
                    } else {
                        blog(LOG_WARNING, "[ShortcutsPortal] Skipping invalid source pointer for hotkey ID %lu", (unsigned long)id);
                    }
                } else if (type == OBS_HOTKEY_REGISTERER_OUTPUT) {
                    name = obs_output_get_name(static_cast<obs_output_t*>(registerer));
                } else if (type == OBS_HOTKEY_REGISTERER_ENCODER) {
                    name = obs_encoder_get_name(static_cast<obs_encoder_t*>(registerer));
                } else if (type == OBS_HOTKEY_REGISTERER_SERVICE) {
                    name = obs_service_get_name(static_cast<obs_service_t*>(registerer));
                }
                
                if (name) {
                    namePrefix = QString::fromUtf8(name);
                }
            }

            if (!namePrefix.isEmpty()) {
                description = QString("[%1] %2").arg(namePrefix, description);
            }

            // Use the unique ID as the key to avoid collisions (e.g. scenes share the same name)
            // Prefix with "hk_" to ensure it doesn't start with a digit, which is invalid for DBus object path elements
            QString uniqueId = "hk_" + QString::number(id);

            t->createShortcut(uniqueId, description, [id](bool pressed) {
                obs_hotkey_trigger_routed_callback(id, pressed);
            });

            return true;
        },
        this
    );
    
    // Clear the temporary pointer set
    m_currentValidSources = nullptr;

    // KDE and Gnome don't allow binding multiple key combinations to the same action like obs does...
    // so add custom "toggle" shortcuts for actions that can be started / stopped

    createShortcut("_toggle_recording", "Toggle Recording", [](bool pressed) {
        // only want this to trigger when we press the bind, not when we release it
        if (!pressed)
            return;

        if (obs_frontend_recording_active()) {
            obs_frontend_recording_stop();
        } else {
            obs_frontend_recording_start();
        }
    });

    createShortcut("_toggle_streaming", "Toggle Streaming", [](bool pressed) {
        if (!pressed)
            return;

        if (obs_frontend_streaming_active()) {
            obs_frontend_streaming_stop();
        } else {
            obs_frontend_streaming_start();
        }
    });

    createShortcut("_toggle_replay_buffer", "Toggle Replay Buffer", [](bool pressed) {
        if (!pressed)
            return;

        if (obs_frontend_replay_buffer_active()) {
            obs_frontend_replay_buffer_stop();
        } else {
            obs_frontend_replay_buffer_start();
        }
    });

    createShortcut("_toggle_virtualcam", "Toggle Virtual Camera", [](bool pressed) {
        if (!pressed)
            return;

        if (obs_frontend_virtualcam_active()) {
            obs_frontend_stop_virtualcam();
        } else {
            obs_frontend_start_virtualcam();
        }
    });

    // https://github.com/obsproject/obs-studio/pull/12580
    /* Update release version number and uncomment when related request is merged.

    if (QVersionNumber::fromString(obs_get_version_string()) >= QVersionNumber(32, 1, 0))
        createShortcut("_toggle_preview", "Toggle Preview", [](bool pressed) {
            if (!pressed)
                return;

            if (obs_frontend_preview_enabled()) {
                obs_frontend_set_preview_enabled(false);
            } else {
                obs_frontend_set_preview_enabled(true);
            }
        });
    */

    createShortcut("_toggle_studio_mode", "Toggle Studio Mode", [](bool pressed) {
        if (!pressed)
            return;

        if (obs_frontend_preview_program_mode_active()) {
            obs_frontend_set_preview_program_mode(false);
        } else {
            obs_frontend_set_preview_program_mode(true);
        }
    });

    struct obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);

    blog(LOG_INFO, "[ShortcutsPortal] Found %lu scenes", (unsigned long)scenes.sources.num);

    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t* source = scenes.sources.array[i];
        const char* name = obs_source_get_name(source);
        QString qName = QString::fromUtf8(name);

        if (qName.isEmpty())
            continue;

        // Use MD5 hash of the scene name to generate a unique, stable, alphanumeric ID
        QString id = "scene_" + QCryptographicHash::hash(qName.toUtf8(), QCryptographicHash::Md5).toHex();

        QString description = "Switch to scene '" + qName + "'";

        createShortcut(id, description, [qName](bool pressed) {
            if (!pressed)
                return;

            obs_source_t* scene = obs_get_source_by_name(qName.toUtf8().constData());
            if (scene) {
                obs_frontend_set_current_scene(scene);
                obs_source_release(scene);
            }
        });
    }
    obs_frontend_source_list_free(&scenes);
}

void ShortcutsPortal::onCreateSessionResponse(unsigned int, const QVariantMap& results)
{
    if (results.contains(u"session_handle"_s)) {
        QString sessionHandle = results[u"session_handle"_s].toString();
        this->m_sessionObjPath = QDBusObjectPath(sessionHandle);
        blog(LOG_INFO, "[ShortcutsPortal] Session created successfully: %s", sessionHandle.toUtf8().constData());
    } else {
        blog(LOG_WARNING, "[ShortcutsPortal] Session creation response did not contain session_handle");
    };

    QDBusConnection::sessionBus().disconnect(
        freedesktopDest,
        m_responseHandle.path(),
        u"org.freedesktop.portal.Request"_s,
        u"Response"_s,
        this,
        SLOT(onCreateSessionResponse(unsigned int, QVariantMap))
    );

    QDBusConnection::sessionBus().connect(
        freedesktopDest,
        freedesktopPath,
        globalShortcutsInterface,
        u"Activated"_s,
        this,
        SLOT(onActivatedSignal(
            QDBusObjectPath, QString, qulonglong, QVariantMap
        ))
    );

    QDBusConnection::sessionBus().connect(
        freedesktopDest,
        freedesktopPath,
        globalShortcutsInterface,
        u"Deactivated"_s,
        this,
        SLOT(onDeactivatedSignal(
            QDBusObjectPath, QString, qulonglong, QVariantMap
        ))
    );

    if (m_isLoaded) {
        createShortcuts();
        bindShortcuts();
    } else {
        blog(LOG_INFO, "[ShortcutsPortal] Deferring shortcut binding until OBS finishes loading");
    }
}

void ShortcutsPortal::onActivatedSignal(
    const QDBusObjectPath&,
    const QString& shortcutName,
    qulonglong,
    const QVariantMap&
)
{
    if (m_shortcuts.contains(shortcutName)) {
        m_shortcuts[shortcutName].callbackFunc(true);
    }
}

void ShortcutsPortal::onDeactivatedSignal(
    const QDBusObjectPath&,
    const QString& shortcutName,
    qulonglong,
    const QVariantMap&
)
{
    if (m_shortcuts.contains(shortcutName)) {
        m_shortcuts[shortcutName].callbackFunc(false);
    }
}

void ShortcutsPortal::bindShortcuts()
{
    blog(LOG_INFO, "[ShortcutsPortal] Binding %d shortcuts...", (int)m_shortcuts.size());
    QDBusMessage bindShortcuts = QDBusMessage::createMethodCall(
        freedesktopDest,
        freedesktopPath,
        globalShortcutsInterface,
        u"BindShortcuts"_s
    );

    QList<QPair<QString, QVariantMap>> shortcuts;

    for (auto shortcut : m_shortcuts) {
        QPair<QString, QVariantMap> dbusShortcut;

        QVariantMap shortcutOptions;
        dbusShortcut.first = shortcut.name;
        shortcutOptions.insert(u"description"_s, shortcut.description);
        dbusShortcut.second = shortcutOptions;

        shortcuts.append(dbusShortcut);
    }

    QMap<QString, QVariant> bindOptions;
    bindOptions.insert(u"handle_token"_s, m_handleToken);

    QList<QVariant> shortcutArgs;
    shortcutArgs.append(m_sessionObjPath);
    shortcutArgs.append(QVariant::fromValue(shortcuts));

    shortcutArgs.append(getWindowId());
    shortcutArgs.append(bindOptions);
    bindShortcuts.setArguments(shortcutArgs);

    QDBusMessage msg = QDBusConnection::sessionBus().call(bindShortcuts);
    if (msg.type() != QDBusMessage::ReplyMessage) {
        auto errMsg = QMessageBox(m_parentWindow);
        errMsg.critical(m_parentWindow, u"Failed to bind shortcuts"_s, msg.errorMessage());
        blog(LOG_ERROR, "[ShortcutsPortal] Failed to bind shortcuts: %s", msg.errorMessage().toUtf8().constData());
    } else {
        blog(LOG_INFO, "[ShortcutsPortal] Shortcuts bound successfully");
    }
}

QString ShortcutsPortal::getWindowId()
{
    // copied from https://invent.kde.org/plasma/plasma-integration/-/blob/20581c0be9357afe052fda94c62c065d298455d9/qt6/src/platformtheme/kioopenwith.cpp#L60-71
    QString windowId;
    QWidget* window = m_parentWindow ? m_parentWindow->window() : nullptr;

    if (!window) {
        // Return an empty ID if the window is not available to prevent crashes.
        return QString();
    }

    window->winId(); // ensure we have a handle so we can export a window (without this windowHandle() may be null)

    auto services = QGuiApplicationPrivate::platformIntegration()->services();
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (auto unixServices = dynamic_cast<QDesktopUnixServices*>(services)) {
#else
    if (auto unixServices = dynamic_cast<QGenericUnixServices*>(services)) {
#endif
        windowId = unixServices->portalWindowIdentifier(window->windowHandle());
    }

    return windowId;
}

void ShortcutsPortal::configureShortcuts()
{
    QDBusMessage bindShortcuts = QDBusMessage::createMethodCall(
        freedesktopDest,
        freedesktopPath,
        globalShortcutsInterface,
        u"ConfigureShortcuts"_s
    );

    QMap<QString, QVariant> bindOptions;
    bindOptions.insert(u"handle_token"_s, m_handleToken);

    QList<QVariant> shortcutArgs;
    shortcutArgs.append(m_sessionObjPath);

    shortcutArgs.append(getWindowId());
    shortcutArgs.append(bindOptions);
    bindShortcuts.setArguments(shortcutArgs);

    QDBusMessage msg = QDBusConnection::sessionBus().call(bindShortcuts);
    if (msg.type() != QDBusMessage::ReplyMessage) {
        auto errMsg = QMessageBox(m_parentWindow);
        errMsg.critical(m_parentWindow, u"Failed to configure shortcuts"_s, msg.errorMessage());
    }
}

ShortcutsPortal::~ShortcutsPortal()
{
    obs_frontend_remove_event_callback(obsFrontendEvent, this);

    QDBusConnection::sessionBus().disconnect(
        freedesktopDest,
        freedesktopPath,
        globalShortcutsInterface,
        u"Activated"_s,
        this,
        SLOT(onActivatedSignal(
            QDBusObjectPath, QString, qulonglong, QVariantMap
        ))
    );
    QDBusConnection::sessionBus().disconnect(
        freedesktopDest,
        freedesktopPath,
        globalShortcutsInterface,
        u"Deactivated"_s,
        this,
        SLOT(onDeactivatedSignal(
            QDBusObjectPath, QString, qulonglong, QVariantMap
        ))
    );
}

void ShortcutsPortal::obsFrontendEvent(enum obs_frontend_event event, void* private_data)
{
    auto* portal = static_cast<ShortcutsPortal*>(private_data);

    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        portal->m_isLoaded = true;
    }

    if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED ||
        event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
        event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
        event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
        
        blog(LOG_INFO, "[ShortcutsPortal] Frontend event received: %d", event);

        if (portal->m_isLoaded && !portal->m_sessionObjPath.path().isEmpty()) {
            // Use invokeMethod to ensure we run on the main thread's event loop
            // and avoid potential race conditions during state changes.
            QMetaObject::invokeMethod(portal, [portal]() {
                portal->createShortcuts();
                portal->bindShortcuts();
            }, Qt::QueuedConnection);
        } else {
            blog(LOG_INFO, "[ShortcutsPortal] Ignoring event, session not yet created or OBS not loaded");
        }
    }
}

#include "moc_shortcutsPortal.cpp"
