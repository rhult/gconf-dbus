#!/bin/bash
#
# Test getting and setting values with all types.
#

# Test integers
gconftool-2 --unset /foo-dbus/bar/int

gconftool-2 --set --type int /foo-dbus/bar/int 69
result=`gconftool-2 --get /foo-dbus/bar/int`
if test "$result" != "69"; then
  echo Expected "69", got $result
  exit 1
fi

gconftool-2 --unset /foo-dbus/bar/int
result=`gconftool-2 --get /foo-dbus/bar/int 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

# Test floats
gconftool-2 --unset /foo-dbus/bar/float

gconftool-2 --set --type float /foo-dbus/bar/float 69.989999999999995
result=`gconftool-2 --get /foo-dbus/bar/float`
if test "$result" != "69.989999999999995"; then
  echo Expected "69.989999999999995", got $result
  exit 1
fi

gconftool-2 --unset /foo-dbus/bar/float
result=`gconftool-2 --get /foo-dbus/bar/float 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

# Test strings
gconftool-2 --unset /foo-dbus/bar/string

gconftool-2 --set --type string /foo-dbus/bar/string båzbözbäz
result=`gconftool-2 --get /foo-dbus/bar/string`
if test "$result" != "båzbözbäz"; then
  echo Expected "båzbözbäz", got $result
  exit 1
fi

gconftool-2 --unset /foo-dbus/bar/string
result=`gconftool-2 --get /foo-dbus/bar/string 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

# Test bools 
gconftool-2 --unset /foo-dbus/bar/bool

gconftool-2 --set --type string /foo-dbus/bar/bool true
result=`gconftool-2 --get /foo-dbus/bar/bool`
if test "$result" != "true"; then
  echo Expected "true", got $result
  exit 1
fi

gconftool-2 --unset /foo-dbus/bar/bool
result=`gconftool-2 --get /foo-dbus/bar/bool 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

