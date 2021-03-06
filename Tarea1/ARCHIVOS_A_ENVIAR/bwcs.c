#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include "jsocket6.4.h"
#include "Data.h"
#include <pthread.h>

char bwc_buffer[BUFFER_LENGTH]; // buffer de TCP a UDP
char bwss_buffer[BUFFER_LENGTH]; // buffer de UDP a TCP

pthread_t bwss_thread; // thread de TCP a UDP
pthread_t bwc_thread; // thread de UDP a TCP

char *bwss_port; // puerto de bwss
char *bwc_port; // puerto de bwc

int bwc_socket;
int bwss_socket;
int ready; // indicara cuando se habra terminado la escritura hacia bwc (retorno del mensaje) para soltar el socket


pthread_mutex_t mutex;
pthread_cond_t cond;

void *connect_client(void *pcl);
void *bwc_connect(void * ptr); 
void *bwss_connect(void *ptr);

/* Funcion encargada de escribir al socket UDP (bwss) */
void DwriteUDP(int cl, char *buf, int l) {
    if(l >= 0) {
	    if(write(cl, buf, l) != l) {
	        perror("falló write en el socket");
	        exit(1);
	    }
    }
	fprintf(stderr, "DwriteUDP: %d bytes \n", l);
}

/* Funcion principal del programa, encargada del proceso de conexion y lanzamiento de los threads */
int main(int argc, char **argv) {
    char *bwss_server;

    if(argc == 1) {
		bwss_server = "localhost";
		bwss_port = "2000";
		bwc_port = "2001";
    } 
    else if (argc == 2) {
    	bwss_server = argv[1];
		bwss_port = "2000";
		bwc_port = "2001";
    } 
    else if(argc == 4) {
    	bwss_server = argv[1];
		bwss_port = argv[2];
		bwc_port = argv[3];
    }
    else {
		fprintf(stderr, "Use: bwcs bwss_server server_port client_port\n");
		return 1;
    }

    ready = 0; // var. ready: Aun no se ha completado proceso de escritura a bwc
    pthread_mutex_init(&mutex,NULL); // inicializacion mutex
    pthread_cond_init(&cond,NULL); // inicializacion condicion

    /* creacion de ambos threads */
    pthread_create(&bwc_thread, NULL, bwc_connect, (void *)bwc_port);  
    pthread_create(&bwss_thread, NULL, bwss_connect, (void *)bwss_server);

    /* Espera a que los threads terminen para finalizar el programa */
    pthread_join(bwc_thread, NULL);
    pthread_join(bwss_thread, NULL);
    return 0;
}

/* Funcion encargada de conectarse por UDP a bwss ademas de realizar el proceso de lectura y escritura
   del paquete de retorno desde bwss a bwc */
void *bwss_connect(void *ptr) {
	int bytes, cnt;

	/* conexion del socket UDP al puerto de BWSS */
	if((bwss_socket = j_socket_udp_connect((char *)ptr, bwss_port)) < 0) {
		printf("udp connect failed\n");
       	exit(1);
	}

	/* Proceso de lectura y escritura desde socket UDP (bwss) a socket TCP (bwc) */
	for(bytes=0;; bytes+=cnt) {
		int size = BUFFER_LENGTH; // largo maximo a leer
	    cnt = read(bwss_socket, bwc_buffer, size); // lectura del socket UDP al buffer correspondiente al cliente
	    
	    fprintf(stderr, "ReadUDP: %d bytes\n", cnt);
		
		if(cnt <= 0) { // identifica EOF de bwss
		  	break;
		}

		Dwrite(bwc_socket, bwc_buffer, cnt); // escritura al socket TCP del cliente

    }
    Dwrite(bwc_socket, bwc_buffer, 0); // escritura de EOF al cliente

    /* Actualizacion variable "ready" para indicar finalizacion del proceso completo */
    pthread_mutex_lock(&mutex);

	ready = 1;
	
	Dclose(bwss_socket); // cierre del socket UDP
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
	
	fprintf(stderr, "SOCKET UDP CERRADO\n");
	return NULL;
}

/* Funcion encargada de aceptar conexion de bwc */
void *bwc_connect(void* ptr) {
	int s, s2;
    int *p;

    s = j_socket_tcp_bind((char *) ptr); // "anclaje" al puerto del bwc 

    if(s < 0) {
	fprintf(stderr, "bind failed\n");
	exit(1);
    }

	s2 = j_accept(s); // aceptacion de la conexion del socket TCP
	p = (int *)malloc(sizeof(int));
	*p = s2;
	connect_client((void *)p); // llamado a la funcion encargada del proceso de lectura y escritura
	return NULL;
}

/*  Funcion encargada de realizar el proceso de lectura y escritura del paquete de retorno desde bwc a bwss  */
void* connect_client(void *pcl){
	int bytes, cnt;
	bwc_socket = *((int *)pcl);
    free(pcl);

	DwriteUDP(bwss_socket, bwss_buffer, 0); // mensaje enviado para "confirmar" conexion con bwss
	
	/* Proceso de lectura y escritura desde bwc a bwss */
	for(bytes=0;; bytes+=cnt) {
        cnt = Dread(bwc_socket, bwss_buffer, BUFFER_LENGTH); // lectura del socket bwc al buffer de bwss
        if(cnt <= 0) break; // condicion de quiebre. Lectura de EOF
        DwriteUDP(bwss_socket, bwss_buffer, cnt); // escritura del buffer de bwss al socket UDP
	}

	DwriteUDP(bwss_socket, bwss_buffer, 0); // escritura de EOF al socket UDP

	pthread_mutex_lock(&mutex);
	while(!ready) // Espera a que haya terminado proceso de retorno de los datos desde bws para finalizar
		pthread_cond_wait(&cond, &mutex);
	Dclose(bwc_socket); // cierre del socket TCP
	pthread_mutex_unlock(&mutex);
	fprintf(stderr, "SOCKET TCP CERRADO\n");
	return NULL;
}
