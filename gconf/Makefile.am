GCONFD_BINARY_NAME=gconfd-2

address=`pkg-config dbus-1 --variable system_bus_default_address`

INCLUDES=								\
	-I$(top_srcdir)							\
	-I$(top_builddir)						\
	$(DEPENDENT_WITH_XML_AND_GTK_CFLAGS) 				\
	$(GCONF_IPC_CFLAGS)						\
	-DG_LOG_DOMAIN=\"GConf\"					\
	-DGCONF_LOCALE_DIR=\""$(gconflocaledir)"\"			\
	-DGCONF_SRCDIR=\""$(absolute_top_srcdir)"\"			\
	-DGCONF_CONFDIR=\""$(sysconfdir)/gconf/$(MAJOR_VERSION)"\"	\
	-DGCONF_ETCDIR=\""$(sysconfdir)/gconf"\"			\
	-DGCONF_BINDIR=\""$(bindir)"\"					\
	-DGCONF_SERVERDIR=\""$(libexecdir)"\"           		\
	-DGCONF_LOCALSTATEDIR=\""$(localstatedir)"\"           		\
	-DGCONF_BUILDDIR=\""$(top_builddir)"\"				\
	-DGCONF_BACKEND_DIR=\""$(pkglibdir)/$(MAJOR_VERSION)"\"		\
	-DVERSION=\""$(VERSION)"\"					\
	-DGCONF_ENABLE_INTERNALS=1					\
	-DGCONFD=\""$(GCONFD_BINARY_NAME)"\"				\
        -DGCONF_SYSTEM_BUS_ADDRESS=\""$address"\"                       \
	-DDBUS_API_SUBJECT_TO_CHANGE=\"1\"

if GTK
SANITY_CHECK=gconf-sanity-check-2
else
SANITY_CHECK=
endif

lib_LTLIBRARIES = libgconf-2.la

bin_PROGRAMS = gconftool-2
libexec_PROGRAMS = gconfd-2 $(SANITY_CHECK)

## not BUILT_SOURCES which seems to make a mess. fix with automake 1.5?
built_sourcecode = 			\
	gconfmarshal.h			\
	gconfmarshal.c

$(libgconf_2_la_OBJECTS) $(gconftool_2_OBJECTS) $(gconfd_2_OBJECTS): $(built_sourcecode)

CLEANFILES = \
	$(built_sourcecode)	\
	stamp-gconfmarshal.h	\
	stamp-gconfmarshal.c	\
	stamp-gconfmarshal.h    \
	stamp-gconfmarshal.c	\
	gconf.service

dist-hook:
	cd $(distdir) ; rm -f $(CLEANFILES)

gconf_headers = 		\
	gconf.h			\
	gconf-changeset.h	\
	gconf-listeners.h	\
	gconf-schema.h		\
	gconf-value.h		\
	gconf-error.h		\
	gconf-engine.h		\
	gconf-client.h

gconfincludedir = $(includedir)/gconf/$(MAJOR_VERSION)/gconf
gconfinclude_HEADERS = 		\
	$(gconf_headers) 	\
	gconf-enum-types.h

gconfd_2_SOURCES = \
	gconf-database.h	\
	gconf-database.c	\
	gconfd-dbus.c		\
	gconfd-dbus.h		\
	gconf-database-dbus.c	\
	gconf-database-dbus.h	\
	gconf-sources.h		\
	gconfd.h		\
	gconfd.c

# these were in the above
#	gconf-dbus-utils.c	
#	gconf-dbus-utils.h	

gconfd_2_LDADD = \
	$(INTLLIBS)		\
	$(DEPENDENT_LIBS) 	\
	$(GCONF_IPC_LIBS)	\
	libgconf-$(MAJOR_VERSION).la

gconftool_2_SOURCES = \
	gconftool.c

gconftool_2_LDADD = \
	$(INTLLIBS)		\
	$(DEPENDENT_WITH_XML_LIBS) \
	$(GCONF_IPC_LIBS) 	\
	$(POPT_LIBS)		\
	libgconf-$(MAJOR_VERSION).la

gconf_sanity_check_2_SOURCES = \
	gconf-sanity-check.c

gconf_sanity_check_2_LDADD = \
	$(INTLLIBS)		\
	$(DEPENDENT_WITH_XML_AND_GTK_LIBS) \
	$(POPT_LIBS)		\
	$(GCONF_IPC_LIBS)	\
	libgconf-$(MAJOR_VERSION).la

libgconf_2_la_SOURCES = \
	gconf-internals.c	\
	gconf-internals.h	\
	gconf.h			\
	gconf.c			\
	gconf-backend.h		\
	gconf-backend.c		\
	gconf-changeset.c	\
	gconf-error.c		\
	gconf-listeners.c	\
	gconf-locale.h  	\
	gconf-locale.c  	\
	gconf-schema.c		\
	gconf-sources.c		\
	gconf-value.c		\
	gconf-client.c		\
	gconf-enum-types.c	\
	gconf-dbus.c 		\
	gconf-dbus-utils.c 	\
	gconf-dbus-utils.h	

libgconf_2_la_LDFLAGS = -version-info $(GCONF_CURRENT):$(GCONF_REVISION):$(GCONF_AGE) -no-undefined
libgconf_2_la_LIBADD = $(INTLLIBS) $(DEPENDENT_LIBS) $(GCONF_IPC_LIBS)

servicedir       = $(DBUS_SERVICE_DIR)
service_in_files = gconf.service.in
service_DATA     = gconf.service

%.service: %.service.in
	sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

if USE_SYSTEM_BUS
systembusconfdir = $(sysconfdir)/dbus-1/system.d
systembusconf_DATA = gconfd.conf
endif

EXTRA_DIST = \
	default.path.in		\
	gconfmarshal.list	\
	regenerate-enum-header.sh \
	regenerate-enum-footer.sh \
	gconf.service.in	\
	gconfd.conf

install-data-local:
	-mkdir -p $(DESTDIR)$(sysconfdir)/gconf/$(MAJOR_VERSION)
	$(INSTALL_DATA) default.path $(DESTDIR)$(sysconfdir)/gconf/$(MAJOR_VERSION)/path

gconfmarshal.h: @REBUILD@ stamp-gconfmarshal.h
	@true

gconfmarshal.c: @REBUILD@ stamp-gconfmarshal.c
	@true

stamp-gconfmarshal.h: @REBUILD@ gconfmarshal.list
	cd $(srcdir) \
	&& glib-genmarshal --prefix=gconf_marshal gconfmarshal.list --header >> xgen-gmh \
	&& (cmp -s xgen-gmh gconfmarshal.h || cp xgen-gmh gconfmarshal.h) \
	&& rm -f xgen-gmh xgen-gmh~

stamp-gconfmarshal.c: @REBUILD@ gconfmarshal.list
	cd $(srcdir) \
	&& glib-genmarshal --prefix=gconf_marshal gconfmarshal.list --body >> xgen-gmc \
	&& (cmp -s xgen-gmc gconfmarshal.c || cp xgen-gmc gconfmarshal.c) \
	&& rm -f xgen-gmc xgen-gmc~

regenerate-built-sources:
	GCONF_SRCDIR=$(srcdir) $(srcdir)/regenerate-enum-header.sh $(gconf_headers)
	GCONF_SRCDIR=$(srcdir) $(srcdir)/regenerate-enum-footer.sh $(gconf_headers)

