/*
 * StateFS BlueZ provider
 *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Denis Zalevskiy <denis.zalevskiy@jollamobile.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 */

#include "provider_bluez.hpp"
#include <iostream>
#include <functional>
#include <cor/util.hpp>
#include <statefs/qt/dbus.hpp>
#include "dbus_types.hpp"

namespace statefs { namespace bluez {

using statefs::qt::Namespace;
using statefs::qt::PropertiesSource;
using statefs::qt::sync;

static char const *service_name = "org.bluez";

Bridge::Bridge(BlueZ *ns, QDBusConnection &bus)
    : PropertiesSource(ns)
    , bus_(bus)
    , watch_(bus, service_name)
{
}

void Bridge::init()
{
    auto setup_manager = [this]() {
        manager_.reset(new Manager(service_name, "/", bus_));
        connect(manager_.get(), &Manager::DefaultAdapterChanged
                , this, &Bridge::defaultAdapterChanged);

        auto getDefault = sync(manager_->DefaultAdapter());
        if (getDefault.isError()) {
            qWarning() << "DefaultAdapter error:" << getDefault.error();
        } else {
            defaultAdapterChanged(getDefault.value());
        }
    };
    auto reset_manager = [this]() {
        manager_.reset();
        static_cast<BlueZ*>(target_)->reset_properties();
    };
    watch_.init(setup_manager, reset_manager);
    setup_manager();
}

void Bridge::defaultAdapterChanged(const QDBusObjectPath &v)
{
    qDebug() << "New default bluetooth adapter" << v.path();
    defaultAdapter_ = v;

    adapter_.reset(new Adapter(service_name, v.path(), bus_));

    sync(adapter_->GetProperties()
         , [this](QVariantMap const &v) {
             setProperties(v);
         });

    connect(adapter_.get(), &Adapter::PropertyChanged
            , [this](const QString &name, const QDBusVariant &value) {
                updateProperty(name, value.variant());
            });

    sync(adapter_->ListDevices()
         , [this](const QList<QDBusObjectPath> &devs) {
             foreach(QDBusObjectPath dev, devs) {
                 addDevice(dev);
             }
         });

    connect(adapter_.get(), &Adapter::DeviceRemoved
            , [this](const QDBusObjectPath &path) {
                removeDevice(path);
            });
    connect(adapter_.get(), &Adapter::DeviceCreated
            , [this](const QDBusObjectPath &path) {
                addDevice(path);
            });
}

void Bridge::addDevice(const QDBusObjectPath &v)
{
    removeDevice(v);

    auto device = cor::make_unique<Device>(service_name, v.path(), bus_);

    sync(device.get()->GetProperties()
         , [this,v](const QVariantMap &props) {
             QVariantMap::const_iterator it = props.find("Connected");
             if (it != props.end()) {
                 if (it.value().toBool())
                     connected_.insert(v);
                 else
                     connected_.erase(v);
                 updateProperty("Connected", connected_.size() > 0);
             }
         });

    connect(device.get(), &Device::PropertyChanged
            , [this,v](const QString &name, const QDBusVariant &value) {
                if (name == QLatin1String("Connected")) {
                    if (value.variant().toBool())
                        connected_.insert(v);
                    else
                        connected_.erase(v);
                    updateProperty("Connected", connected_.size() > 0);
                }
            });

    devices_.insert(std::make_pair(v, std::move(device)));
}

void Bridge::removeDevice(const QDBusObjectPath &v)
{
    auto it = devices_.find(v);
    if (it != devices_.end())
        devices_.erase(it);
    if (connected_.erase(v))
        updateProperty("Connected", connected_.size() > 0);
}

BlueZ::BlueZ(QDBusConnection &bus)
    : Namespace("Bluetooth", std::unique_ptr<PropertiesSource>
                (new Bridge(this, bus)))
    , defaults_({
            { "Enabled", "0" }
            , { "Visible", "0" }
            , { "Connected", "0" }
            , { "Address", "00:00:00:00:00:00" }})
{
    addProperty("Enabled", "0", "Powered");
    addProperty("Visible", "0", "Discoverable");
    addProperty("Connected", "0");
    addProperty("Address", "00:00:00:00:00:00");
    src_->init();
}

void BlueZ::reset_properties()
{
    setProperties(defaults_);
}


class Provider : public statefs::AProvider
{
public:
    Provider(statefs_server *server)
        : AProvider("bluez", server)
        , bus_(QDBusConnection::systemBus())
    {
        auto ns = std::make_shared<BlueZ>(bus_);
        insert(std::static_pointer_cast<statefs::ANode>(ns));
    }
    virtual ~Provider() {}

    virtual void release() {
        delete this;
    }

private:
    QDBusConnection bus_;
};

static Provider *provider = nullptr;

static inline Provider *init_provider(statefs_server *server)
{
    if (provider)
        throw std::logic_error("provider ptr is already set");
    registerDataTypes();
    provider = new Provider(server);
    return provider;
}

}}

EXTERN_C struct statefs_provider * statefs_provider_get
(struct statefs_server *server)
{
    return statefs::bluez::init_provider(server);
}
