set(PARAGRAPH_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../paragraph.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PARAGRAPH_SOURCE}")

set(DISPLAY_FS_DIR "${CMAKE_CURRENT_BINARY_DIR}/display_fs")
file(MAKE_DIRECTORY "${DISPLAY_FS_DIR}")
configure_file("${PARAGRAPH_SOURCE}" "${DISPLAY_FS_DIR}/paragraph.txt" COPYONLY)

idf_component_register(
                         SRCS "main.c" 
                              "LCD_Driver/Vernon_ST7789T/Vernon_ST7789T.c" 
                              "LCD_Driver/ST7789.c"
                              "LVGL_Driver/LVGL_Driver.c"
                              "LVGL_UI/display.c"
                              "LVGL_UI/display_words.c"

                         INCLUDE_DIRS 
                              "./LCD_Driver/Vernon_ST7789T" 
                              "./LCD_Driver" 
                              "./LVGL_Driver" 
                              "./LVGL_UI" 
                              "."
                         REQUIRES esp_lcd spiffs
                       )

spiffs_create_partition_image(flash_test "${DISPLAY_FS_DIR}" FLASH_IN_PROJECT)
