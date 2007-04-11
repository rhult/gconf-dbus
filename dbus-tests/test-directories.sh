#!/bin/bash
#
# Test listing.
#

# Test listing values 
gconftool-2 --recursive-unset /foo/bar
gconftool-2 --set --type int /foo/bar/int 69
gconftool-2 --set --type float /foo/bar/float 69.989999999999995
gconftool-2 --set --type string /foo/bar/string baz
gconftool-2 --set --type bool /foo/bar/bool true

result=`gconftool-2 --recursive-list /foo/bar|wc -l`
if test $result -ne 4; then
  echo Expected 4 items to be set in /foo/bar, got $result
  exit 1
fi

# Test directory exists
gconftool-2 --recursive-unset /foo/bar
gconftool-2 --set --type int /foo/bar/baz/int 69
gconftool-2 --dir-exists=/foo/bar/baz

if test $? -eq 2; then
  echo Expected /foo/bar/baz to exist but it does not
  exit 1
fi

result=`gconftool-2 --all-dirs /foo/bar|wc -l`
if test "$result" != "1"; then
  echo Expected /foo/bar to have just ONE directory, got $result 
  exit 1
fi

# Test directory unsetting
gconftool-2 --unset /foo/bar/baz/int
gconftool-2 --dir-exists=/foo/bar/baz

if test $? -eq 0; then
  echo Expected /foo/bar/baz to NOT exist but it does
  exit 1
fi

# Test recursive directory unsetting
gconftool-2 --recursive-unset /foo
gconftool-2 --dir-exists=/foo

if test $? -eq 0; then
  echo Expected /foo to NOT exist but it does
  exit 1
fi
