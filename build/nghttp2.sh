	echo "Checking for nghttp2 library ..."

	NGHTTP2INCDIR=""
	NGHTTP2LIBDIR=""
	for DIR in /opt/nghttp2* /usr/local/nghttp2* /usr/local /usr/pkg /opt/csw /opt/sfw /opt/homebrew /opt/local
	do
		if test -f $DIR/include/nghttp2/nghttp2.h
		then
			NGHTTP2INCDIR=$DIR/include
		fi

		if test -f $DIR/lib/libnghttp2.so
		then
			NGHTTP2LIBDIR=$DIR/lib
		fi
		if test -f $DIR/lib/libnghttp2.dylib
		then
			NGHTTP2LIBDIR=$DIR/lib
		fi
		if test -f $DIR/lib/libnghttp2.a
		then
			NGHTTP2LIBDIR=$DIR/lib
		fi
		if test -f $DIR/lib64/libnghttp2.so
		then
			NGHTTP2LIBDIR=$DIR/lib64
		fi
		if test -f $DIR/lib64/libnghttp2.a
		then
			NGHTTP2LIBDIR=$DIR/lib64
		fi
	done

	if test "$USERNGHTTP2INC" != ""; then
		NGHTTP2INCDIR="$USERNGHTTP2INC"
	fi
	if test "$USERNGHTTP2LIB" != ""; then
		NGHTTP2LIBDIR="$USERNGHTTP2LIB"
	fi

	# Lets see if it builds
	NGHTTP2OK="YES"
	cd build
	if test "$NGHTTP2INCDIR" != ""; then INCOPT="-I$NGHTTP2INCDIR"; fi
	if test "$NGHTTP2LIBDIR" != ""; then LIBOPT="-L$NGHTTP2LIBDIR"; fi
	OS=`uname -s | sed -e's@/@_@g'` $MAKE -f Makefile.test-nghttp2 nghttp2-clean
	OS=`uname -s | sed -e's@/@_@g'` NGHTTP2INC="$INCOPT" $MAKE -f Makefile.test-nghttp2 test-compile
	if test $? -eq 0; then
		echo "Compiling with nghttp2 library works OK"
	else
		echo "Cannot compile with nghttp2 library"
		NGHTTP2OK="NO"
	fi

	if test "$NGHTTP2OK" = "YES"
	then
		OS=`uname -s | sed -e's@/@_@g'` NGHTTP2LIB="$LIBOPT" $MAKE -f Makefile.test-nghttp2 test-link
		if test $? -eq 0; then
			echo "Linking with nghttp2 library works OK"
		else
			echo "Cannot link with nghttp2 library"
			NGHTTP2OK="NO"
		fi
		OS=`uname -s | sed -e's@/@_@g'` $MAKE -f Makefile.test-nghttp2 nghttp2-clean
	fi
	cd ..

	if test "$NGHTTP2OK" = "NO"; then
		echo "The nghttp2 library was not found or is not usable."
		echo "xymonnet will be built without HTTP/2 test support."
		echo "nghttp2 can be found at https://nghttp2.org/"
		echo ""
		echo "If you have nghttp2 installed, use the \"--nghttp2include DIR\" and"
		echo "\"--nghttp2lib DIR\" options to configure to specify where it is."
		echo ""
	fi
