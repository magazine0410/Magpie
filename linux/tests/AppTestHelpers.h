#pragma once
#include <QStringList>

// The warnings and errors Qt has reported since the last call; a QML file
// with a mistake shows up here
QStringList TakeQtWarnings();
