# ceres-devices - los periféricos.
#
# Terminal, temporizador, control del sistema, disco y framebuffer: todo lo que implementa
# IODevice y habla con el host. Separado de la VM para que añadir un periférico no obligue a
# tocar la CPU, y para que una construcción sin terminal ni disco sea posible.
#
# Es el corte más fácil de posponer de los cinco: si estorba, sus fuentes pueden volver a
# ceres-vm sin que nada más cambie.

ceres_add_library(devices DEPENDS core vm)
ceres_add_tests(devices)
