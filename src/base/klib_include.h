// Opens a KenshiLib include block. The library's headers (and the Ogre, MyGUI
// and Boost headers under them) raise three warnings the mod cannot fix:
// C4482, an enum name used as a qualifier; C4005, a macro defined twice; and
// C4099, a type declared as a class and defined as a struct. Include this,
// then the library headers, then base/klib_include_end.h. There is no include
// guard: every block opens its own warning scope.
#pragma warning(push)
#pragma warning(disable: 4482 4005 4099)
