#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "ESP32-targz.h"
#include <Arduino.h>

#define  SD_CS_PIN 5
#define RXD2 16
#define TXD2 17

const char* ssid = "112358";
const char* password = "Wawen69$";
String gzFilePath = ""; // Inicialmente vacío, se actualizará dinámicamente 
String outputFilePath = "/RondanaMini.gcode"; // The output file or directory

// Initialize the TarGzUnpacker object
TarGzUnpacker tarGz;

WebServer server(80);

void handleRoot() {
  File htmlFile = SD.open("/index.html", FILE_READ); // Open the HTML file on the SD card
  if (!htmlFile) {
    server.send(404, "text/plain", "File Not Found");
    return;
  }

  // Send the HTML content as the HTTP response
  server.streamFile(htmlFile, "text/html");

  htmlFile.close(); // Close the file after sending its content
}

// Logger callback function
void targzPrintLoggerCallback(const char* format, ...) {
  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  Serial.print(buf);
}

void handleFileDelete() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Bad Request: No file name provided");
    return;
  }
  String filename = server.arg("file");
  if (!SD.exists(filename)) {
    server.send(404, "text/plain", "File Not Found");
    return;
  }

  String nogzFilename;
  if (filename.endsWith(".gz")) {
    nogzFilename = filename.substring(0, filename.length() - 3); // Elimina los últimos 3 caracteres (.gz)
  } else {
    nogzFilename = filename;
  }
  
  SD.remove(filename);
  SD.remove(nogzFilename);
  
  server.send(200, "text/plain", "File Deleted Successfully");
  
  // Aquí debes enviar un mensaje de actualización a la página web
  server.send(200, "text/plain", "File Deleted Successfully");

  // Vuelve a cargar la lista de archivos en la interfaz
  server.send(200, "text/plain", "Reload");
}

void handleFileUpload() {
  if (server.uri() != "/upload") return;
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = "/sd" + upload.filename;
    if (!SD.exists("/sd")) {
      SD.mkdir("/sd");
    }
    File file = SD.open(filename, FILE_WRITE);
    if(!file){
      Serial.println("Error al abrir el archivo para escribir");
      return;
    }
    Serial.printf("Subida de archivo: %s\n", filename.c_str());
    file.close();
    gzFilePath = filename;

    int lastDotIndex = gzFilePath.lastIndexOf('.');
    if (lastDotIndex != -1) {
      outputFilePath = gzFilePath.substring(0, lastDotIndex);
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    String filename = "/sd" + upload.filename;
    File file = SD.open(filename, FILE_APPEND);
    if(file){
      file.write(upload.buf, upload.currentSize);
      file.close();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    String message = "Archivo Subido Exitosamente: " + upload.filename + ", Tamaño: " + String(upload.totalSize) + " bytes";
    server.send(200, "text/plain", message);
    Serial.println(message);
    
    tarGz.setLoggerCallback(targzPrintLoggerCallback);
    bool decompressResult = tarGz.gzExpander(SD, gzFilePath.c_str(), SD, outputFilePath.c_str());
    if (decompressResult) {
      Serial.println("Decompression succeeded.");
    } else {
      Serial.print("Decompression failed with error: ");
      Serial.println(tarGz.tarGzGetError());
    }
  }
}

void handleFileList() {
  File root = SD.open("/");
  String fileList = "[";
  bool first = true;
  while (File file = root.openNextFile()) {
    if (file.isDirectory()) {
      continue;
    }
    if (!first) {
      fileList += ",";
    }
    fileList += "\"" + String(file.name()) + "\"";
    first = false;
  }
  fileList += "]";
  server.send(200, "application/json", fileList);
}

void handlePrint() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Bad Request: No file name provided");
        return;
    }

    String filenameStr = server.arg("file");
    char filename[filenameStr.length() + 1];
    filenameStr.toCharArray(filename, sizeof(filename));

    if (strstr(filename, ".gz") != NULL) {
        filename[strlen(filename) - 3] = '\0'; // Elimina la extensión .gz para obtener el nombre del archivo real
    }

    if (!SD.exists(filename)) {
        Serial.println("Archivo no encontrado");
        server.send(404, "text/plain", "File Not Found");
        return;
    }

    File file = SD.open(filename, FILE_READ);
    if (!file) {
        Serial.println("Fallo al abrir el archivo para imprimir");
        server.send(500, "text/plain", "Failed to open file for printing");
        return;
    }

    Serial.println("Iniciando impresión...");

    char line[512];
    bool okReceived = false;

    while (file.available()) {
        size_t len = file.readBytesUntil('\n', line, sizeof(line) - 1);
        line[len] = '\0'; // Asegura que la cadena esté correctamente terminada

        // Ignora líneas de comentario o líneas vacías
        if (line[0] == ';' || strlen(line) == 0) {
            continue; // Salta al siguiente ciclo del bucle
        }

        // Intenta enviar el comando y recibir "ok"
        okReceived = false;
        int attempt = 0;
        const int maxAttempts = 3; // Número máximo de intentos para reestablecer la comunicación
        
        while (!okReceived && attempt < maxAttempts) {
            Serial2.println(line);
            Serial.printf("Intento %d - Enviando: %s\n", attempt + 1, line);

            unsigned long startTime = millis();
            while (millis() - startTime < 5000) { // Espera hasta 5 segundos por respuesta
                if (Serial2.available()) {
                    String serialResponse = Serial2.readStringUntil('\n');
                    if (serialResponse.indexOf("ok") != -1) {
                        okReceived = true;
                        Serial.println("Respuesta OK recibida");
                        break; // Sal de este bucle ya que recibiste un "ok"
                    }
                }
            }

            if (okReceived) {
                break; // Sal de este bucle ya que recibiste un "ok"
            } else {
                Serial.println("No se recibió respuesta, intentando reestablecer la comunicación...");
                attempt++;
                if(attempt < maxAttempts){
                    Serial2.end(); // Cierra la comunicación Serial2
                    delay(100); // Pequeña pausa para dar tiempo al hardware
                    Serial2.begin(250000, SERIAL_8N1, RXD2, TXD2); // Reestablece la comunicación Serial2
                }
            }
        }

        if (!okReceived) {
            Serial.println("Error: No se pudo reestablecer la comunicación con la impresora.");
            file.close();
            server.send(500, "text/plain", "Failed to communicate with the printer.");
            return;
        }
    }

    file.close();
    server.send(200, "text/plain", "Trabajo de impresión completado exitosamente");
    Serial.println("Trabajo de impresión completado exitosamente");
}



String urlDecode(String str) {
    String decoded = "";
    char temp[] = "0x00";
    unsigned int len = str.length();
    unsigned int i = 0;
    while (i < len) {
        if (str[i] == '+') {
            decoded += ' ';
        } else if (str[i] == '%') {
            if (i + 2 < len) {
                temp[2] = str[i + 1];
                temp[3] = str[i + 2];
                decoded += (char)strtol(temp, NULL, 16);
                i += 2;
            }
        } else {
            decoded += str[i];
        }
        i++;
    }
    return decoded;
}

void setup(void) {

  Serial.begin(115200); // Para la depuración
  Serial2.begin(250000, SERIAL_8N1, RXD2, TXD2); // Configura el Serial2 a 250000 baudios, que es común en impresoras 3D

  if(!SD.begin()){
    Serial.println("Error al montar la tarjeta SD");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi conectado.");
  Serial.print("Dirección IP del servidor: ");
  Serial.println(WiFi.localIP()); // Imprime la dirección IP del servidor

  server.on("/", handleRoot);
  server.on("/listFiles", HTTP_GET, handleFileList);
  server.on("/delete", HTTP_GET, handleFileDelete);
  server.on("/print", HTTP_GET, handlePrint);
  server.on("/home", HTTP_GET, []() {
    Serial2.println("G28 ;");
    server.send(200, "text/plain", "Enviando impresora a Home");
    Serial.println("Enviando G28 a la impresora");
  });

  // Ruta para mover hacia la derecha.
server.on("/derecha", HTTP_GET, []() {
    Serial2.println("G91 ; Establecer coordenadas relativas");
    Serial2.println("G1 X1 F1000 ; Mover 1mm a la derecha");
    Serial2.println("G90 ; Establecer coordenadas absolutas");
    server.send(200, "text/plain", "Moviendo 1mm a la derecha");
    Serial.println("Enviando G1 X1 a la impresora");
});

// Ruta para mover hacia la izquierda.
server.on("/izquierda", HTTP_GET, []() {
    Serial2.println("G91 ; Establecer coordenadas relativas");
    Serial2.println("G1 X-1 F1000 ; Mover 1mm a la izquierda");
    Serial2.println("G90 ; Establecer coordenadas absolutas");
    server.send(200, "text/plain", "Moviendo 1mm a la izquierda");
    Serial.println("Enviando G1 X-1 a la impresora");
});

server.on("/arriba", HTTP_GET, []() {
    Serial2.println("G91 ; Establecer coordenadas relativas");
    Serial2.println("G1 Y1 F1000 ; Mover 1mm a enfrente");
    Serial2.println("G90 ; Establecer coordenadas absolutas");
    server.send(200, "text/plain", "Moviendo 1mm a la derecha");
    Serial.println("Enviando G1 X1 a la impresora");
});

// Ruta para mover hacia la izquierda.
server.on("/abajo", HTTP_GET, []() {
    Serial2.println("G91 ; Establecer coordenadas relativas");
    Serial2.println("G1 Y-1 F1000 ; Mover 1mm a la izquierda");
    Serial2.println("G90 ; Establecer coordenadas absolutas");
    server.send(200, "text/plain", "Moviendo 1mm a la izquierda");
    Serial.println("Enviando G1 X-1 a la impresora");
});

server.on("/z-mas", HTTP_GET, []() {
    Serial2.println("G91 ; Establecer coordenadas relativas");
    Serial2.println("G1 Z1 F1000 ; Mover 1mm a la derecha");
    Serial2.println("G90 ; Establecer coordenadas absolutas");
    server.send(200, "text/plain", "Moviendo 1mm a la derecha");
    Serial.println("Enviando G1 X1 a la impresora");
});

// Ruta para mover hacia la izquierda.
server.on("/z-menos", HTTP_GET, []() {
    Serial2.println("G91 ; Establecer coordenadas relativas");
    Serial2.println("G1 Z-1 F1000 ; Mover 1mm a la izquierda");
    Serial2.println("G90 ; Establecer coordenadas absolutas");
    server.send(200, "text/plain", "Moviendo 1mm a la izquierda");
    Serial.println("Enviando G1 X-1 a la impresora");
});

// Repite la estructura para arriba, abajo, z-mas, z-menos ajustando los comandos del código G.

  server.on("/upload", HTTP_POST, [](){ 
    server.send(200, "text/plain", ""); 
  }, handleFileUpload);
  server.on("/download", HTTP_GET, []() {
    
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Bad Request: No file name provided");
      return;
    }
    String filename = server.arg("file"); // Asume que los archivos están en un directorio 'sd'
    if (!SD.exists(filename)) {
      server.send(404, "text/plain", "File Not Found");
      return;
    }

    File file = SD.open(filename, FILE_READ);
    if (!file) {
      server.send(500, "text/plain", "Failed to open file");
      return;
    }

    server.streamFile(file, "application/octet-stream");
    file.close();
  });
  server.onNotFound([](){
    server.send(404, "text/plain", "404: Not Found");
  });

  server.begin();
  Serial.println("Servidor HTTP iniciado");

}

void loop(void) {
  server.handleClient();
}