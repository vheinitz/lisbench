# Install rules and the Debian package (cpack -G DEB). Only the component
# "lisbench" is packaged: the fetched dependencies bring install rules of
# their own (headers, static libraries) that do not belong in the package.

include(GNUInstallDirs)

install(TARGETS lisbench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT lisbench)
install(DIRECTORY docs/devices/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lisbench/devices
        COMPONENT lisbench PATTERN TEMPLATE.md EXCLUDE)
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples COMPONENT lisbench)
install(FILES README.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lisbench)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME copyright COMPONENT lisbench)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES packaging/lisbench.service DESTINATION lib/systemd/system COMPONENT lisbench)
  install(FILES packaging/lisbench.default DESTINATION /etc/default RENAME lisbench
          COMPONENT lisbench)
endif()

set(CPACK_PACKAGE_NAME lisbench)
set(CPACK_PACKAGE_VENDOR "Valentin Heinitz")
set(CPACK_PACKAGE_CONTACT "Valentin Heinitz <vheinitz@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "workbench for laboratory instrument interfaces (CLSI LIS01-A2 / LIS02-A2)")
set(CPACK_PACKAGE_DESCRIPTION
    "Simulate, monitor, bridge, map and conformance-test links between laboratory\n instruments and a LIS (formerly ASTM E1381/E1394), with a web UI. A hobby\n project for development and integration, not for clinical use.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/vheinitz/lisbench")
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};lisbench;lisbench;/")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)

if(NOT LISBENCH_DEB_ARCH)
  execute_process(COMMAND dpkg --print-architecture OUTPUT_VARIABLE LISBENCH_DEB_ARCH
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${LISBENCH_DEB_ARCH}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION science)
# Built on Debian 13 (GCC 14): its C and C++ runtimes. Listed by hand, since
# dpkg-shlibdeps does not work across architectures without more setup.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.36), libstdc++6 (>= 14), libgcc-s1, adduser")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${PROJECT_SOURCE_DIR}/packaging/deb/postinst;${PROJECT_SOURCE_DIR}/packaging/deb/prerm;${PROJECT_SOURCE_DIR}/packaging/deb/postrm;${PROJECT_SOURCE_DIR}/packaging/deb/conffiles")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_STRIP_FILES ON)

include(CPack)
