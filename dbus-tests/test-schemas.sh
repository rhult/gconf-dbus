#!/bin/bash
#
# Test schemas.
#

file=test-schemas.schemas

# Remove any previous data that might have already existed 
gconftool-2 --recursive-unset /schemas/foo
gconftool-2 --recursive-unset /foo

# Check we have no values already existing
gconftool-2 --dir-exists /schemas/foo
if test $? -eq 0; then
  echo The path /schemas/foo is not supposed to exist but it does.
  exit 1
fi

gconftool-2 --dir-exists /foo
if test $? -eq 0; then
  echo The path /foo is not supposed to exist but it does.
  exit 1
fi

# Check we have a schemas file
if test ! -f $file; then
  echo Could not find schemas file:$file for testing
  exit 1
fi

# Install new schema
gconftool-2 --install-schema-file $file

# Test setting a string 
gconftool-2 --set --type string /foo/bar/string sliff

result=`gconftool-2 --get /foo/bar/string 2>/dev/null`
if test "$result" != "sliff"; then
  echo Expected /foo/bar/string to be "sliff", it is $result
  exit 1
fi

# Test unsetting the string returns it to the default
gconftool-2 --unset /foo/bar/string 

result=`gconftool-2 --get /foo/bar/string 2>/dev/null`
if test "$result" != "baz"; then
  echo Expected /foo/bar/string to be "baz" \(the default\), it is $result
  exit 1
fi

# Clean up
gconftool-2 --recursive-unset /schemas/foo
gconftool-2 --recursive-unset /foo

# Test there is no default value
result=`gconftool-2 --get /foo/bar/string 2>/dev/null`
if test -n "$result"; then
  echo Expected /foo/bar/string to be empty, it is $result
  exit 1
fi


