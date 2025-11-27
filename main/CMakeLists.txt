idf_component_register(SRCS "hackingtool.c"
    INCLUDE_DIRS "."
                    )
target_link_libraries(${COMPONENT_LIB} -Wl,-zmuldefs)



