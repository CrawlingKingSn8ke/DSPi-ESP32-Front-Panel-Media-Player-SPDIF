# libsmb2

Source: https://github.com/sahlberg/libsmb2
Pinned commit: a52c1571a1ff223bde5bb3df9fec7af5ce3855d4
Licence: LGPL-2.1-or-later; see COPYING, LICENCE-LGPL-2.1.txt and file notices.

The upstream lib C translation units are stored as .impl.h files;
small .c wrappers supply the upstream ESP build definitions locally. This avoids
changing global Arduino compiler flags or leaking a generic config.h into the
rest of the application. Public/private headers are flattened from upstream
lib, include and include/smb2. Two public-header smb2/ angle-bracket includes
are changed to local quoted includes for this layout, as are the two local
includes in each of alloc, timestamps and unicode. All other implementation
content is unchanged. config.h is upstream
include/esp/config.h.

Excluded: Kerberos wrapper and Apple AES backend. This build uses NTLMSSP and
the portable AES implementation, with SMB2/3 signing negotiation. No SMB1,
Kerberos, SMB-over-QUIC, RDMA or separate libdcerpc integration is enabled.

Keep the complete pinned source available with any distributed firmware and
provide the LGPL-required relinking materials/instructions before publication.
This is a local integration stage, not a published release.
