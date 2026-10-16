#pragma once
#include <QString>
namespace Party {
// Bootstrap private writable data from the package, preserving usable user files.
int seedGeoData(const QString &work, const QString &resourceDirectory = {});
}
