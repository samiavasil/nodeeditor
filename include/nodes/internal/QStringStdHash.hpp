#pragma once

#include <functional>

#include <QtCore/QString>
#include <QtCore/QVariant>

namespace std
{
// Qt provides std::hash<QString> in supported versions.
// Keep this namespace block intentionally empty to avoid redefinition.
}
