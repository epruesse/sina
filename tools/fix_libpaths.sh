#!/bin/bash
set -e

UNAME=$(uname)

logrun() {
    echo "$@"
    "$@"
}

get_rpaths_Darwin() {
    otool -l "$1" | grep LC_RPATH -A2 |grep path | awk '{print $2}'
}
get_rpaths_Linux() {
    patchelf --print-rpath "$1" | tr : '\n' | sed 's|$ORIGIN|'$ORIGIN'|'
}
get_rpaths() {
    get_rpaths_$UNAME "$@"
}

get_needed_Darwin() {
    otool -L "$1" |grep -v CoreFoundation | tail -n +2 | awk '{print $1}'
}
get_needed_Linux() {
    patchelf --print-needed "$1"
}
get_needed() {
    get_needed_$UNAME "$@"
}

remove_rpaths_Darwin() {
    for rpath in $(get_rpaths_Darwin $1); do
	logrun install_name_tool -delete_rpath $rpath "$1"
    done
}
remove_rpaths_Linux() {
    logrun patchelf --remove-rpath "$1"
}
remove_rpaths() {
    remove_rpaths_$UNAME "$@"
}

set_rpath_Darwin() {
    remove_rpaths_Darwin "$1"
    logrun install_name_tool -add_rpath "$2" "$1"
}
set_rpath_Linux() {
    logrun patchelf --set-rpath "$2" "$1"
}
set_rpath() {
    set_rpath_$UNAME "$@"
}

copy_libs_needed() {
    echo "Checking libs for $1"
    ORIGIN=`dirname $1`
    rpaths=$(get_rpaths $1)
    self=`echo ${1##*/} | sed 's/\+/\\\\+/g'`
    dtneed=$(get_needed $1 | grep -vE "^(/usr/lib|/System)|$self" || true)
    dest=`realpath $(dirname $1)/../lib`
    recurse=
    for lib in $dtneed; do
	#echo "  checking for $lib"
	name="${lib#@rpath/}"
	if test -e "$dest/$name"; then
	    continue;
	fi
	source=
	for path in "" $rpaths $LDPATHS; do
	    if test -e "$path/$name"; then
		source="$path/$name"
		break;
	    fi
	done
	if test -z "$source"; then
	    echo Missing $name
	else
	    echo "$source -> $dest/$name"
	    cp $source $dest/$name
	    recurse="$recurse $dest/$name"
	fi
    done
    for lib in $recurse; do
	copy_libs_needed $lib
    done
}
		    

make_rpath_relative_Darwin() {
    set_rpath_Darwin "$1" @loader_path/../lib
}
make_rpath_relative_Linux() {
    set_rpath_Linux "$1" '$ORIGIN/../lib/arb/lib:$ORIGIN/../lib'
}
make_rpath_relative() {
    make_rpath_relative_$UNAME "$@"
}

make_absolute_path_relative_Darwin() {
    for lib in $(get_needed_Darwin $1); do
	if test ${lib:0:1} = "/"; then
	    name=$(basename $lib)
	    dir="$(dirname $lib)"
	    if test "$dir" != "/usr/lib"; then
		logrun install_name_tool -change $lib @rpath/$name -add_rpath $dir "$1"
	    fi
	fi
    done
}
make_absolute_path_relative_Linux() {
    :
}
make_absolute_path_relative() {
    make_absolute_path_relative_$UNAME "$@"
}

if test "$0" != "-bash"; then
    if test -n "$1"; then
	echo Fixing libs for "$1"
	make_absolute_path_relative "$1"
	copy_libs_needed "$1"
	make_rpath_relative "$1"
    fi
fi
