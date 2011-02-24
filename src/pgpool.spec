%define short_name	pgpool

Summary:	Pgpool is a connection pooling/replication server for PostgreSQL
Name:		postgresql-%{short_name}
Version:	3.4
Release:	1%{?dist}
License:	BSD
Group:		Applications/Databases
URL:		http://pgpool.projects.PostgreSQL.org
Source0:	http://pgfoundry.org/frs/download.php/1374/%{short_name}-%{version}.tar.gz
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

%description
pgpool is a connection pooling/replication server for PostgreSQL.
pgpool runs between PostgreSQL's clients(front ends) and servers
(backends). A PostgreSQL client can connect to pgpool as if it 
were a standard PostgreSQL server.

%prep
%setup -q -n %{short_name}-%{version}

%build
CFLAGS="${CFLAGS:-%optflags}" ; export CFLAGS
CXXFLAGS="${CXXFLAGS:-%optflags}" ; export CXXFLAGS

%configure --with-pam

make %{?smp_flags}

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} install
install -m 755 %{short_name} %{buildroot}%{_bindir}
install -m 644 %{short_name}.8 %{buildroot}%{_mandir}/man8/
install -d %{buildroot}/%{_docdir}/%{name}-%{version}
mv %{buildroot}%{_sysconfdir}/pgpool.conf.sample %{buildroot}/%{_docdir}/%{name}-%{version}
mv %{buildroot}%{_sysconfdir}/pool_hba.conf.sample %{buildroot}/%{_docdir}/%{name}-%{version}

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%doc README README.euc_jp TODO COPYING INSTALL AUTHORS ChangeLog NEWS pgpool.conf.sample pool_hba.conf.sample
%{_datadir}/pgpool/pgpool.pam
%{_bindir}/pgpool
%{_mandir}/man8/*


%changelog
* Wed Aug 1 2007 - Devrim GUNDUZ <devrim@commandprompt.com> 3.4-1
- Update to 3.4
- Removed patches, they are now in upstream

* Sat Jun 2 2007 - Devrim GUNDUZ <devrim@commandprompt.com> 3.3-2
- Spec file version bump up for 3.3

* Mon May 21 2007 - Devrim GUNDUZ <devrim@commandprompt.com> 3.3-1
- Update to 3.3
- Added temporary patches from upstream, will be removed in next
release.

* Mon Feb 12 2007 - Devrim GUNDUZ <devrim@commandprompt.com> 3.2-1
- Update to 3.2

* Tue Jan 16 2007 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.2-2
- Removed vendor tag, per rh bugzilla #222797

* Tue Dec 12 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.2-1
- Update to 3.1.2-1

* Tue Dec 5 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.1-7
- Better fix for conf file problem, per bugzilla review

* Fri Nov 28 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.1-6
- Truncate configure line

* Fri Nov 24 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.1-5
- moved sample conf file to %%doc
- Renamed package to postgresql-pgpool

* Fri Sep 8 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.1-4
- Fix changelog date
- Remove dependency for postgresql-server. 

* Mon Jul 31 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.1-3
- Removed --bindir from configure, per bugzilla review (#199679)

* Fri Jul 23 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.1-2
- Fixed spec file, per bugzilla review (#199679)

* Fri Jul 23 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.1-1
- Update to 3.1.1

* Fri Jul 22 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.0-2
- Fixed spec file, per bugzilla review (#199679)

* Fri Jul 21 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.1.0-1
- Update to 3.1.0-1
- Fixed rpmlint errors

* Thu May 25 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.0.2
- Update to 3.0.2

* Thu Feb 05 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 3.0.0
- Update to 3.0.0 for PgPool Global Development Group

* Thu Feb 02 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 2.7.2-1
- Update to 2.7.2

* Thu Jan 26 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 2.7.1-1
- Update to 2.7.1

* Sun Jan 15 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 2.7-1
- Update to 2.7

* Wed Dec 28 2005 Devrim Gunduz <devrim@commandprompt.com> pgpool-2.6.5
- Update to 2.6.5
- Removed post scripts
- Updated doc files

* Sat Oct 22 2005 Devrim Gunduz <devrim@PostgreSQL.org> pgpool-2.6.4
- Update to 2.6.4
