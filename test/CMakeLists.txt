project(hyp_udmabuf_test)

cmake_minimum_required(VERSION 3.10)

set (LIBKIUMD_INC_PATH ${CMAKE_CURRENT_LIST_DIR}/../drivers/include/uapi)

set(SOURCE_FILES
  hyp_udmabuf_test.c
)

add_executable (hyp_udmabuf_test ${SOURCE_FILES})

include_directories (${LIBKIUMD_INC_PATH})

target_link_libraries (hyp_udmabuf_test kiumd)

install(TARGETS hyp_udmabuf_test  ARCHIVE DESTINATION lib  LIBRARY DESTINATION lib  COMPONENT library)
install(TARGETS "hyp_udmabuf_test" DESTINATION bin)


