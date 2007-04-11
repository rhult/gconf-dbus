#!/bin/bash
#
# Test getting and setting values with all types.
#

# Test integers
gconftool-2 --unset /foo/bar/int

gconftool-2 --set --type int /foo/bar/int 69
result=`gconftool-2 --get /foo/bar/int`
if test "$result" != "69"; then
  echo Expected "69", got $result
  exit 1
fi

gconftool-2 --unset /foo/bar/int
result=`gconftool-2 --get /foo/bar/int 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

# Test floats
gconftool-2 --unset /foo/bar/float

gconftool-2 --set --type float /foo/bar/float 69.989999999999995
result=`gconftool-2 --get /foo/bar/float`
if test "$result" != "69.989999999999995"; then
  echo Expected "69.989999999999995", got $result
  exit 1
fi

gconftool-2 --unset /foo/bar/float
result=`gconftool-2 --get /foo/bar/float 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

# Test strings
gconftool-2 --unset /foo/bar/string

gconftool-2 --set --type string /foo/bar/string båzbözbäz
result=`gconftool-2 --get /foo/bar/string`
if test "$result" != "båzbözbäz"; then
  echo Expected "båzbözbäz", got $result
  exit 1
fi

gconftool-2 --unset /foo/bar/string
result=`gconftool-2 --get /foo/bar/string 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

# Test bools 
gconftool-2 --unset /foo/bar/bool

gconftool-2 --set --type string /foo/bar/bool true
result=`gconftool-2 --get /foo/bar/bool`
if test "$result" != "true"; then
  echo Expected "true", got $result
  exit 1
fi

gconftool-2 --unset /foo/bar/bool
result=`gconftool-2 --get /foo/bar/bool 2>/dev/null`
if test -n "$result"; then
  echo Expected unset value got $result
  exit 1
fi

