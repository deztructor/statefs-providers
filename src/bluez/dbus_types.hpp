#ifndef _STATEFS_PROVIDER_BLUEZ_COMMON_H_
#define _STATEFS_PROVIDER_BLUEZ_COMMON_H_

#include <QList>
#include <QString>
#include <QtCore/QMetaType>
#include <QtDBus/QtDBus>

#include <tuple>
#include <statefs/qt/dbus.hpp>

typedef std::tuple<unsigned, QString> BluezService;
Q_DECLARE_METATYPE(BluezService);

typedef QList<BluezService> ServiceMap;
Q_DECLARE_METATYPE(ServiceMap);

static inline void registerDataTypes()
{
    qDBusRegisterMetaType<BluezService>();
    qDBusRegisterMetaType<ServiceMap>();
}


#endif // _STATEFS_PROVIDER_BLUEZ_COMMON_H_
