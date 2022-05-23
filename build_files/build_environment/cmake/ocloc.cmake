# SPDX-License-Identifier: GPL-2.0-or-later

set(OCLOC_EXTRA_ARGS
)

ExternalProject_Add(external_ocloc
  URL file://${PACKAGE_DIR}/${OCLOC_FILE}
  URL_HASH ${OCLOC_HASH_TYPE}=${OCLOC_HASH}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  PREFIX ${BUILD_DIR}/ocloc
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${LIBDIR}/ocloc ${DEFAULT_CMAKE_FLAGS} ${OCLOC_EXTRA_ARGS}
  INSTALL_DIR ${LIBDIR}/ocloc
)
