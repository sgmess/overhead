idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "../../include"
    REQUIRES waveshare__esp32_s3_touch_lcd_28c
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC OVERHEAD_BOARD_S3_28C=1)
