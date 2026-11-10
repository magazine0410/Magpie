// What the view models share. A view model is the data of one page of the
// user interface, as on Windows: QML reads and writes its properties, and it
// reads and writes the settings.
//
// All properties of a view model have the one change signal Changed: a page
// has few bindings, and evaluating them all again costs nothing that could
// be noticed.
#pragma once
#include <QObject>

// A property that is a member of the settings: its getter, and a setter that
// saves. `expr` is an lvalue.
#define MAGPIE_SETTING(type, Name, expr) \
	type Name() const { return (type)(expr); } \
	void Set##Name(type value) { \
		if ((type)(expr) == value) { \
			return; \
		} \
		(expr) = (decltype(expr))value; \
		_Save(); \
	}
