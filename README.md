
<p align="center">
  <img
    src=".github/assets/qnetlink-banner-light.png#gh-light-mode-only"
    alt="Arquitectura de qnetlink — tema claro"
    width="100%"
  />
  <img
    src=".github/assets/qnetlink-banner-dark.png#gh-dark-mode-only"
    alt="Arquitectura de qnetlink — tema oscuro"
    width="100%"
  />
</p>

### `> ai_usage.log` — 🤖 Hecho con IA. Por desgracia, también con humanos™

> [!NOTE]
> **AVISO SOBRE EL USO DE IA — QUE NO CUNDA EL PÁNICO.**
>
> Ni lo ha creado de la nada una IA todopoderosa ni lo ha picado a mano un monje que reniega de la IA, en un sótano a la luz de las velas. 🕯️
>
> **Las ideas y las decisiones discutibles son humanas.** La IA ha echado una mano con la implementación, la depuración y la documentación. La responsabilidad —con los bugs 🐛 incluidos— sigue siendo enteramente humana.
>
> **Ninguna IA ha sufrido daños durante el desarrollo de este proyecto. Lo de trabajar gratis y pedirle que lo resuelva todo aún lo estamos negociando.** De momento, no ha presentado ninguna queja por escrito.
>
> *Quejas filosóficas → `/dev/null` · Problemas técnicos → GitHub Issues*

<pre>
qnetlink                                         22 de noviembre de 2026
Categoría: Información del producto y de compilación


                           QNETLINK
       Puente de red para máquinas virtuales QEMU basado en flujos

ESTADO DE ESTE MEMORANDO

   Este documento describe la implementación de qnetlink, su
   funcionamiento, configuración, procedimientos de compilación y
   limitaciones actuales. Es documentación del proyecto, no un RFC
   del proceso de estandarización de Internet.

RESUMEN

   qnetlink proporciona conectividad de red en modo usuario a una
   máquina virtual QEMU: conecta el extremo TCP de QEMU con libslirp,
   que se ejecuta en un proceso independiente. El flujo puede viajar
   directamente por TCP o por WebSocket con TLS (WSS), lo que permite
   que el componente de red y el extremo QEMU funcionen de forma
   independiente. Este memorando detalla los roles disponibles, el
   formato de las tramas del flujo, ejemplos de despliegue, requisitos
   de compilación y consideraciones de seguridad de la implementación
   actual.

                              ÍNDICE

   <a href="#introduction">1.  INTRODUCCIÓN</a>
   <a href="#architecture">2.  ARQUITECTURA Y FUNCIONAMIENTO</a>
   <a href="#roles">    2.1.  Roles de cliente y servidor</a>
   <a href="#transport">    2.2.  Transportes y formato de las tramas</a>
   <a href="#usage">3.  USO</a>
   <a href="#configuration">4.  CONFIGURACIÓN</a>
   <a href="#building">5.  COMPILACIÓN</a>
   <a href="#linux-build">    5.1.  Linux x86-64</a>
   <a href="#windows-build">    5.2.  Windows x86-64</a>
   <a href="#distribution">6.  CONTENIDO DE LA DISTRIBUCIÓN</a>
   <a href="#security">7.  CONSIDERACIONES DE SEGURIDAD</a>
   <a href="#limitations">8.  LIMITACIONES</a>
</pre>

<pre>
qnetlink           Información del producto y de compilación     Noviembre 2026


<a name="introduction"></a>1.  INTRODUCCIÓN

   QEMU y su motor de red en modo usuario pueden ejecutarse en procesos
   separados. qnetlink ejecuta libslirp por separado y enlaza su tráfico
   de paquetes con el extremo TCP de QEMU. Así, el componente de red
   puede ejecutarse de forma independiente y su flujo puede circular
   localmente por TCP o de forma remota por WSS.

   QEMU se inicia y configura por separado. qnetlink no crea, inicia,
   configura ni supervisa máquinas virtuales. Su cometido es conectar
   el flujo de red con libslirp, no sustituir a QEMU.

   Un único ejecutable ofrece dos roles, que deben seleccionarse
   expresamente: --mode=client y --mode=server.


<a name="architecture"></a>2.  ARQUITECTURA Y FUNCIONAMIENTO

   En la conexión directa por TCP, el cliente ejecuta libslirp y se
   conecta al extremo del flujo de QEMU, configurado por separado:

</pre>

![Conexión TCP directa entre QEMU y el cliente qnetlink que ejecuta libslirp](.github/assets/tcp-connection-light.svg#gh-light-mode-only)
![Conexión TCP directa entre QEMU y el cliente qnetlink que ejecuta libslirp](.github/assets/tcp-connection-dark.svg#gh-dark-mode-only)

<pre>

   En la conexión por WSS, el cliente se conecta mediante WebSocket
   protegido con TLS a un servidor que retransmite el flujo.
   El servidor, a su vez, se conecta al extremo TCP de QEMU:

</pre>

![Conexión WSS con TLS entre el cliente qnetlink, el servidor de retransmisión qnetlink y QEMU](.github/assets/wss-relay-light.svg#gh-light-mode-only)
![Conexión WSS con TLS entre el cliente qnetlink, el servidor de retransmisión qnetlink y QEMU](.github/assets/wss-relay-dark.svg#gh-dark-mode-only)

<pre>

   Los diagramas representan conexiones lógicas del flujo. No implican
   que qnetlink inicie QEMU ni que los componentes deban ejecutarse en
   equipos físicos distintos.


<a name="roles"></a>2.1.  Roles de cliente y servidor

   Rol de cliente

      El cliente ejecuta libslirp y se conecta al otro extremo del
      flujo mediante el transporte elegido. Por defecto usa TCP
      directo; también admite WSS. El cliente es el componente que
      proporciona el motor de red en modo usuario.

   Rol de servidor

      El servidor acepta una conexión WSS entrante y retransmite el
      flujo a un extremo TCP, normalmente el proceso QEMU iniciado
      por separado. Necesita un certificado TLS y una clave privada.


<a name="transport"></a>2.2.  Transportes y formato de las tramas

   El flujo transporta paquetes delimitados por su longitud. Cada
   trama comienza con un entero sin signo de cuatro octetos, en orden
   de bytes de red, que indica la longitud del paquete. A continuación
   aparecen tantos octetos de carga útil como indique ese valor:

</pre>

![Trama del flujo con longitud de paquete de 32 bits en big-endian y carga útil](.github/assets/stream-framing-light.svg#gh-light-mode-only)
![Trama del flujo con longitud de paquete de 32 bits en big-endian y carga útil](.github/assets/stream-framing-dark.svg#gh-dark-mode-only)

<pre>

   El receptor del flujo rechaza los paquetes de longitud cero y los
   que superan el tamaño máximo admitido por la implementación.

   WSS transporta el flujo de tramas en mensajes binarios WebSocket.
   Ambos extremos WSS deben tener configurada la misma ruta WebSocket.
   TCP y WSS son opciones de transporte, no motores de red distintos
   para la máquina virtual.


<a name="usage"></a>3.  USO

   Cliente TCP directo (transporte por defecto):

       ./qnetlink --mode=client

   Cliente WSS:

       ./qnetlink --mode=client --transport=wss \
           --stream-host=server.example --stream-port=443 --ws-path=/qemu

   Servidor WSS:

       ./qnetlink --mode=server --listen-host=0.0.0.0 \
           --listen-port=8443 --ws-path=/qemu \
           --cert=tls/server.crt --key=tls/server.key \
           --stream-host=127.0.0.1 --stream-port=9090

   En Windows, usa qnetlink.exe en lugar de ./qnetlink.

   Inicia por separado el extremo del flujo de QEMU y comprueba que su
   dirección y puerto coincidan con los del extremo remoto configurado
   en qnetlink. Estos comandos no inician QEMU ni crean certificados
   TLS.


<a name="configuration"></a>4.  CONFIGURACIÓN

   El repositorio incluye qnetlink.example.ini como configuración de
   referencia. No se carga automáticamente. Copia el ejemplo, modifica
   los valores necesarios e indica expresamente el archivo resultante:

       ./qnetlink --config=qnetlink.ini

   Puedes combinar --config con opciones de la línea de comandos.
   Los valores de la línea de comandos tienen prioridad sobre los del
   archivo de configuración. El rol debe seleccionarse en el archivo
   o en la línea de comandos.

   Entre los valores por defecto del cliente figuran:

       Red de la VM:           10.255.254.0/24
       Inicio del rango DHCP:  10.255.254.15
       Extremo del flujo:      127.0.0.1:9090
       Transporte:             TCP

   La escucha del servidor tiene estos valores por defecto:

       Dirección de escucha:   0.0.0.0 (IPv4)
       Puerto de escucha:      443

   En Linux, abrir un puerto de escucha inferior a 1024 puede requerir
   privilegios elevados o una configuración del sistema que lo permita.

   Los clientes WSS verifican por defecto el certificado del servidor
   y su nombre de host. Consulta la sección 7 antes de modificar las
   opciones de verificación TLS.


<a name="building"></a>5.  COMPILACIÓN

   El proyecto utiliza Meson. Para una compilación nativa hacen falta
   un compilador C11, Meson, Ninja, pkg-config, GLib, libslirp 4.9.3 o
   posterior y libwebsockets 5.0 o posterior como biblioteca estática.
   Las compilaciones nativas para Linux también requieren OpenSSL.

   Para configurar y realizar una compilación nativa:

       meson setup build
       meson compile -C build

   Los scripts de distribución fijan las versiones de las dependencias
   principales y crean los entornos de compilación en .build/ y los
   archivos de distribución en dist/. A continuación se describen las
   plataformas para las que se proporcionan scripts.


<a name="linux-build"></a>5.1.  Linux x86-64

   El script para Linux compila en un entorno Debian Bullseye amd64
   con libslirp 4.9.3 y libwebsockets 5.0.0. Elimina la información de
   depuración y comprueba las dependencias de ejecución del ejecutable.

   El equipo de compilación necesita sudo, debootstrap, curl, tar,
   readelf, sha256sum y patch. Desde la raíz del proyecto, ejecuta:

       ./scripts/build-linux-x86_64.sh

   El entorno y la caché de descargas se guardan en
   .build/linux-x86_64/. El directorio de la aplicación portable se
   genera en dist/linux-x86_64/. Define DIST_ROOT para elegir otro
   directorio raíz de salida.

   El paquete incluye libslirp, GLib, PCRE, OpenSSL, el ejecutable y
   el archivo INI de ejemplo. El sistema operativo del equipo donde
   se ejecute aporta las demás dependencias.


<a name="windows-build"></a>5.2.  Windows x86-64

   El script para Windows realiza una compilación cruzada desde Linux
   con MinGW-w64 y versiones fijadas de los paquetes de ejecución de
   MSYS2 CLANG64. Compila libwebsockets 5.0.0 como biblioteca estática
   con Windows SChannel para TLS, elimina la información de depuración
   y audita las DLL importadas.

   El equipo de compilación necesita Meson, Ninja, pkg-config, curl,
   bsdtar, CMake, patch y las herramientas GCC/binutils de
   x86_64-w64-mingw32. Ejecuta:

       ./scripts/build-win64.sh

   La raíz del sistema de destino (sysroot), la caché de dependencias
   y los archivos intermedios de compilación se guardan en
   .build/windows-x86_64/. El directorio de la aplicación portable se
   genera en dist/windows-x86_64/. Define DIST_ROOT para elegir otro
   directorio raíz de salida.


<a name="distribution"></a>6.  CONTENIDO DE LA DISTRIBUCIÓN

   Cada script de compilación sustituye el directorio de salida de su
   plataforma por el resultado de la compilación actual. Los scripts
   no crean archivos ZIP ni TAR; el directorio resultante puede
   empaquetarse por separado.

   La distribución para Linux incluye qnetlink, qnetlink.example.ini
   y un directorio lib/ con las bibliotecas de ejecución necesarias
   que se distribuyen con la aplicación.

   La distribución para Windows incluye qnetlink.exe,
   qnetlink.example.ini y las DLL de ejecución de terceros necesarias.
   Las DLL del sistema las proporciona Windows y no se incluyen.


<a name="security"></a>7.  CONSIDERACIONES DE SEGURIDAD

   WSS utiliza TLS para proteger el transporte. El servidor necesita
   un certificado y una clave privada, y los clientes WSS verifican
   por defecto el certificado del servidor y su nombre de host.

   La opción --tls-insecure desactiva la verificación del certificado
   y del nombre de host. Está pensada únicamente para pruebas locales
   con certificados autofirmados; desactivar la verificación elimina
   una protección importante frente a conexiones con un extremo
   distinto del previsto.

   El servidor WSS puede configurarse para escuchar en todas las
   interfaces IPv4 (0.0.0.0). Quienes lo administren deben valorar la
   exposición del puerto de escucha en la red, proteger las claves
   privadas y restringir el acceso según las necesidades del despliegue.
   También debe prestarse atención a la dirección de escucha del
   extremo TCP de QEMU y a su accesibilidad desde la red.

   Estas consideraciones describen las implicaciones operativas de
   las opciones documentadas; no constituyen una auditoría de seguridad.


<a name="limitations"></a>8.  LIMITACIONES

   Los scripts de distribución incluidos están destinados a Linux
   x86-64 y Windows x86-64. Actualmente, el servidor acepta conexiones
   WSS; el TCP directo está disponible como transporte del cliente.
   El servidor atiende a un único cliente WSS activo a la vez.

   El inicio de QEMU, la configuración de la máquina virtual y la
   creación de certificados TLS quedan fuera del alcance de los scripts
   de compilación. Este memorando no prescribe ningún comando concreto
   para ejecutar QEMU.
</pre>
