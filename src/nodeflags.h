#pragma once
#include <QString>

namespace Party {
// Recognition uses the subscription's name, never a network geolocation request.
QString nodeCountryCode(const QString &name);
QString nodeFlagPath(const QString &name);
QString nodeDisplayName(const QString &name);
}
