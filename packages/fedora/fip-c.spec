Name:           fip-c
Version:        0.3.2
Release:        1%{?dist}
Summary:        C Interop Module utilizing the Flint Interop Protocol

License:        MIT
URL:            https://github.com/flint-lang/fip
Source0:        https://github.com/flint-lang/fip/releases/download/v%{version}/fip-c

%description
C Interop Module utilizing the Flint Interop Protocol

%prep
# No preparation, the binareis are published directly

%build
# No build needed, we ship prebuilt static binaries

%install
install -Dm755 %{SOURCE0} %{buildroot}%{_bindir}/fip-c

%files
%{_bindir}/fip-c

%changelog
* Wed Jun 10 2026 Marc Zweiler marc.zweiler@outlook.at - 0.3.2-1
- Initial release for COPR
