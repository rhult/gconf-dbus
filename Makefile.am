
SUBDIRS = gconf backends po doc examples
DIST_SUBDIRS=tests $(SUBDIRS)

DBUS_GCONF_CONF=gconfd.conf

if USE_SYSTEM_BUS
dbusconfdir=$(sysconfdir)/dbus-1/system.d
dbusconf_DATA = $(DBUS_GCONF_CONF)
endif

EXTRA_DIST=gconf.m4.in TODO \
	   gconf-2.0.pc.in  \
	   $(DBUS_GCONF_CONF)

install-data-local:
	-mkdir -p $(DESTDIR)$(datadir)/aclocal
	$(INSTALL_DATA) gconf.m4 $(DESTDIR)$(datadir)/aclocal/gconf-$(MAJOR_VERSION).m4
	-mkdir -p $(DESTDIR)$(sysconfdir)/gconf/gconf.xml.defaults
	-chmod 755 $(DESTDIR)$(sysconfdir)/gconf/gconf.xml.defaults
	-mkdir -p $(DESTDIR)$(sysconfdir)/gconf/gconf.xml.mandatory
	-chmod 755 $(DESTDIR)$(sysconfdir)/gconf/gconf.xml.mandatory

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = gconf-2.0.pc

install-schemas:
	(cd standard-schemas && $(MAKE) $(AM_MAKEFLAGS) install-schemas)
