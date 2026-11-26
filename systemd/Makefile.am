DISTCLEANFILES = Makefile.in gluster-blockd.service gluster-block-target.service \
                 gluster-blockd.initd gluster-block-apid.service

CLEANFILES = *~ gluster-blockd.service gluster-block-target.service \
             gluster-blockd.initd gluster-block-apid.service

EXTRA_DIST = gluster-blockd.service.in gluster-block-target.service.in \
             gluster-blockd.initd.in gluster-blockd.sysconfig \
             gluster-block-apid.service.in gluster-block-apid.sysconfig

if USE_SYSTEMD
# systemddir is already defined through configure.ac
systemd_DATA = gluster-blockd.service gluster-block-target.service
if BUILD_API
systemd_DATA += gluster-block-apid.service
endif
else
# initddir is already defined through configure.ac
initd_DATA = gluster-blockd.initd
endif

install-data-local:
	$(MKDIR_P) $(DESTDIR)${sysconfigdir};              \
	$(INSTALL_DATA) gluster-blockd.sysconfig           \
		$(DESTDIR)${sysconfigdir}/gluster-blockd;
if BUILD_API
	$(INSTALL_DATA) $(srcdir)/gluster-block-apid.sysconfig \
		$(DESTDIR)${sysconfigdir}/gluster-block-apid;
endif

uninstall-local:
	rm -f $(DESTDIR)${sysconfigdir}/gluster-blockd
	rm -f $(DESTDIR)${sysconfigdir}/gluster-block-apid
