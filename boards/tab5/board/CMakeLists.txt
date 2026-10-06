idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "../../include"
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC OVERHEAD_BOARD_TAB5=1)
