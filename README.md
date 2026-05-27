[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/Y-iVkroM)
# CosmiKernel

![logo](assets/logo.png)

## Introducción

Estás al mando de una nave espacial minera que explora asteroides en busca de recursos. Las estaciones espaciales del sector demandan distintos recursos y pagan bien por ellos. Debes cuidar tu combustible, lo necesitas tanto para trasladarte como para extraer los recursos. Y manten un ojo en el nivel de oxígeno de la nave.

## Objetivos

El objetivo de este trabajo es aplicar en el desarrollo de un juego diversos conceptos de sistemas operativos como gestión de procesos e hilos, sincronización, comunicación entre procesos, entrada/salida y sistemas de archivos.

## Descripción general del juego

El juego se desarrolla en un mapa de caracteres bidimensional que representa el espacio profundo. En el escenario se encuentran:

- **Naves espaciales**: exploran en el espacio en busca de asteroides con recursos. Cada nave gasta combustible al desplazarse y al extraer recursos. También consumen oxígeno de forma periódica.
- **Estaciones espaciales**: son los centros que demandan recursos. Las naves acuden a ellas para vender lo recolectado. También allí pueden recargar combustible y reabastecer su tanque de oxígeno. Al igual que las naves, consumen combustible de manera periódica. Cuando el nivel de combustible baje de cierto umbral, envían un mensaje a todas las naves del cuadrante solicitando provisiones de deuterio.
- **Asteroides**: están distribuidos de forma aleatoria en el mapa y contienen una cantidad abundante, pero finita, de recursos valiosos que las naves deben extraer. Los asteroides son ricos en:
    - *Deuterio*: el combustible de las naves espaciales y las estaciones espaciales.
    - *Mutexio*, *semaforita* y *kernelio*: minerales con múltiples usos como generación de oxígeno, preparacion de aleaciones ultra-resistentes o condimento para pizzas.
    - Sin embargo, no todos los asteroides tienen que tener necesariamente los cuatro minerales, ni la misma cantidad de cada uno.

## Arquitectura

El proyecto debe ser multiproceso y multihilo. La arquitectura es cliente-servidor. El mapa es administrado por un proceso servidor, que debe llevar el registro de donde estan los asteroides, las naves espaciales y las estaciones espaciales. Los clientes son las naves y las estaciones espaciales.

## Gestión de procesos e hilos

- Administración del mapa del sector espacial: un proceso servidor debe administre el mapa: recursos, ubicación de las naves y estaciones, etc.

- Estaciones: cada estación es un proceso independiente (cliente), que se conecta al servidor que administra el mapa.
    - Las estaciones cuentan con un hangar donde reciben a las naves espaciales. El máximo de naves que pueden albergar es de 3.
    - Deben consumir combustible de manera periódica.
    - Realizan transacciones de compra/venta con las naves espaciales (reciben recursos, entregan combustible y oxígeno).

- Naves: Cada nave es un proceso independiente (cliente) que se conecta al servidor que administra el mapa. Deben contar con hilos para:
    - Soporte vital: administra el nivel de oxígeno de la nave. Periódicamente debe decrementarlo.
    - Propulsión: se encarga de mover la nave a través del mapa (detecta pulsaciones de teclado, gasta combustible en el movimiento).
    - Extracción: maneja la maquinaria necesaria para la extracción de recursos (gasta combustible).
    - Radar: visualiza y actualiza el mapa en base a la información que provee el servidor.

## Memoria compartida

El escenario, que es un mapa de caracteres bidemensional, es administrado por un proceso servidor. El mapa debe residir en una memoria compartida POSIX, de manera que los clientes pueden acceder al mismo fácilmente para su visualización.

## Sincronización

- **Mutexs**: para actualizar las estructuras de datos internas de cada nave, asteroides, estaciones espaciales, etc.
- **Semáforos binarios**: cada celda del mapa tiene asociado un semáforo binario, de manera que dos naves no puedan ocupar el mismo lugar.
- **Semáforos contadores**: las estaciones espaciales sólo pueden recibir como máximo 3 naves en sus hangares.

## Paso de mensajes

Las transacciones con una estación espacial deben realizarse mediante mensajes POSIX. Estas operaciones son la venta de los recursos obtenidos y recarga de combustible y oxígeno, por un precio justo. Sólo se pueden realizar cuando una nave se encuentra en el hangar de la estación espacial.

Las comunicaciones entre la estación espacial y las naves en el cuadrante también deben ser realizadas mediante mensajes POSIX.

## Sistema de archivos

Se debe interactuar con el sistema de archivos para:
- Configuración inicial: los servidores deben contar con un archivo de configuración inicial (`config.txt`) que determine el número de estaciones espaciale en el mapa (máximo 3), número de asteroides presentes al mismo tiempo, precios de los minerales, combustible y oxígeno.
- Bitácora: las transacciones de compra y venta de las estaciones espaciales deben ser registradas de manera atómica en una bitácora de eventos.

## Entrada / Sálida

- Capturar las pulsaciones de las teclas para el movimiento de las naves (arriba, abajo, izquierda, derecha) por ejemplo las teclas `wasd` o `hjkl`.
- Presentar el mapa del área que comparte el servidor, en una terminal, usando la librería `ncurses`.

## Reglas del juego

- Cuando el combustible o el oxígeno de una nave llega a cero, la tripulación queda incapacitada y la nave desactivada. En ese estado puede ser presa de otra nave minera que puede obtener así los recursos recolectados, su combustible y oxígeno. El programa cliente debe indicar un "game over", pero el servidor mantiene la nave para que sus recursos puedan ser aprovechados por otros jugadores.
- El combustible de las estaciones espaciales también puede llegar a cero. En ese caso la estación espacial queda desactivada, lo que es una catástrofe. El juego finaliza en caso de que las estaciones espaciales queden sin combustible.

## Otras consideraciones

- Queda a criterio de cada grupo como diseñar la interfaz de usuario de los programas clientes (naves y estaciones). Mínimamente, el cliente debe permitir visualizar el cuadrante, permitir mover la nave e interactuar con los asteroides y las estaciones espaciales. Para estos casos es posible que se deba visualizar información adicional (recursos disponibles en el asteroide, precios de recursos en la estación espacial, aviso de que el hangar esta ocupado, etc.)
- Si el servidor con el escenario termina su ejecución de manera normal, debe guardar su estado y avisar a los clientes (naves y estaciones). Estos deben notificar al jugador de la desconexión. Los procesos clientes pueden guardar su estado, pero sólo sería válido para el escenario almacenado.

## Criterios de evaluación

- El código debe estar debidamente modularizado, comentado y organizado.
- El sistema no debe generar abrazos mortales, dejar recursos huérfanos, corromper archivos, etc.
- Los integrantes del grupo deben poder explicar, justificar y defender el diseño e implementación de cualquier parte del sistema.
- Debe compilar sin *warnings*.

## Adicionales

Pueden agregar las siguientes u otras, características adicionales al juego:

- Movimiento de los asteroides: los asteroides pueden aparecer y desaparecer del mapa siguiendo una trayectoria y distintas velocidades.
- Combate: las naves pueden atacar otras mediante misiles que tienen un cierto rango de alcance, medido en celdas en una cierta dirección.
    - Cada nave tiene un nivel de escudo.
    - Al disparar un misil, la nave debe "tomar" un mutex en la nave atacada, que le permite decrementar su escudo.
    - Si el escudo llega a cero, la nave queda averiada y puede ser presa de cualquier otra nave.
- Agujeros negros: singularidades que atraen naves si estan cerca o las atrapa si caen dentro de su horizonte de sucesos. El consumo de combustible se incrementa dentro del área cercana al agujero negro. Si la nave no se mueve, es atraída al mismo. Si cae en el agujero, la nave se pierde junto con todos sus recursos.

