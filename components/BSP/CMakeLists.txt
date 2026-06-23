set(src_dirs
            LED
            KEY
            BLUETOOTH
            IIC
            SPI)
set(include_dirs
            LED
            KEY
            BLUETOOTH
            IIC
            SPI)
set(requires
            driver
            nvs_flash
            bt)
idf_component_register(SRC_DIRS ${src_dirs}
                      INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
