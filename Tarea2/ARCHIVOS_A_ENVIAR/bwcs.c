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
#include <time.h>

char bwc_buffer[BUFFER_LENGTH + DHDR]; // buffer entre bwss y bwcs (UDP)
char bwss_buffer[BUFFER_LENGTH]; // buffer entre bwc y bwcs (TCP)
char final_bwssbuff[BUFFER_LENGTH + DHDR]; // buffer entre bwcs y bwss (UDP)
char final_bwcbuff[BUFFER_LENGTH]; // buffer entre bwcs y bwc (TCP)

pthread_t bwss_thread; // thread de TCP a UDP
pthread_t bwc_thread; // thread de UDP a TCP

char *bwss_port; // puerto de bwss
char *bwc_port; // puerto de bwc

int bwc_socket;
int bwss_socket;
int ready; // indicara cuando se habra terminado la escritura hacia bwc (retorno del mensaje) para soltar el socket

int ack_bit; // Indica el numero de secuencia del ACK que se espera
int frame_seq; // Indica el numero de secuencia del paquete que 
int timeout; // Es el timeout que se utilizara
int received_ack; // Indica si se recibe el ACK;

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

/* Funcion encargada de obtener el numero de secuencia del header UDP */
int getNumSeq(char *buf) {
    int res=0;
    int i;

    for(i=DSEQ; i < DHDR; i++)
        res = (res*10)+(buf[i]-'0');

    return res;
}

/* Funcion encargada de copiar informacion desde buffer UDP/TCP a buffer TCP/UDP */
void copyArray(int fromOrig, int fromFinal, int toFinal, char *original, char *final) {
	int i;
	int range = toFinal - fromFinal;
	for(i = 0; i < range; i++) {
		final[i + fromFinal] = original[i + fromOrig];
	}
}

/* Funcion encargada de agregar el numero de secuencia al header UDP */
void addNumSeq(int frameSeq, char *buf) {
	int i;
	int res = frameSeq;
	for(i = DHDR - 1; i >= DSEQ ; i--) {
		buf[i] = (res % 10) + '0';
        res /= 10;
	}
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

    ack_bit = 0; // Indica numero de secuencia del ACK enviado. Parte en el 0
    frame_seq = 0; // Indica numero de secuencia del paquete enviado. Parte en el 0
    timeout = 1; // Indica tiempo de espera por un paquete. 1 segundo
    received_ack = 0; // Indica si se recibio el ack del paquete enviado.

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
	int receivedSeqNum;
	int lastReceivedSeqNum = -1; // permite descartar archivos duplicados


	/* conexion del socket UDP al puerto de BWSS */
	if((bwss_socket = j_socket_udp_connect((char *)ptr, bwss_port)) < 0) {
		printf("udp connect failed\n");
       	exit(1);
	}

	/* Proceso de lectura y escritura desde socket UDP (bwss) a socket TCP (bwc) */
	for(bytes=0;; bytes+=cnt) {
		int size = BUFFER_LENGTH + DHDR; // largo maximo a leer
	    cnt = read(bwss_socket, bwc_buffer, size); // lectura del socket UDP al buffer correspondiente al cliente

	    receivedSeqNum = getNumSeq(bwc_buffer);

	    if(bwc_buffer[0] == 'A') {
	    	if(receivedSeqNum == ack_bit) {
		    	received_ack = 1;
		    	continue;
		    }
	    } else if(bwc_buffer[0] == 'D') {
	    	memset(final_bwssbuff, 0, BUFFER_LENGTH + DHDR);
	    	final_bwssbuff[0] = 'A';
	    	addNumSeq(receivedSeqNum, final_bwssbuff);

	    	//mando antes a tcp
	    	if(cnt - DHDR <= 0) { // identifica EOF de bwss
		  		break;
			}
			DwriteUDP(bwss_socket, final_bwssbuff, DHDR); 
			if(lastReceivedSeqNum != receivedSeqNum) { // Evita escribir a bwc paquetes repetidos
				copyArray(DHDR, 0, cnt - DHDR, bwc_buffer, final_bwcbuff);
				Dwrite(bwc_socket, final_bwcbuff, cnt - DHDR); // escritura al socket TCP del cliente 
			}
	    	lastReceivedSeqNum  = receivedSeqNum;
 	  	}
	    
	    fprintf(stderr, "ReadUDP: %d bytes\n", cnt);
		
    }
    DwriteUDP(bwss_socket, final_bwssbuff, DHDR);
	copyArray(DHDR, 0, cnt - DHDR, bwc_buffer, final_bwcbuff);	
    Dwrite(bwc_socket, final_bwcbuff, 0); // escritura de EOF al cliente

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
	clock_t t, tEnd;
	bwc_socket = *((int *)pcl);
    free(pcl);

	DwriteUDP(bwss_socket, bwss_buffer, 0); // mensaje enviado para "confirmar" conexion con bwss
	
	/* Proceso de lectura y escritura desde bwc a bwss */
	for(bytes=0;; bytes+=cnt) {
        cnt = Dread(bwc_socket, bwss_buffer, BUFFER_LENGTH); // lectura del socket bwc al buffer de bwss
        if(cnt <= 0) break; // condicion de quiebre. Lectura de EOF

        // DEBO MODIFICAR BUFFER PARA CONTENER HEADER
        final_bwssbuff[DTYPE] = 'D';
        addNumSeq(frame_seq, final_bwssbuff);
        copyArray(0, DHDR, BUFFER_LENGTH + DHDR, bwss_buffer, final_bwssbuff);
       
        ack_bit = frame_seq; // ACK que espera como confirmacion 
        frame_seq = (frame_seq + 1) % MAX_SEQ; // Proximo numero de secuencia a enviar

        DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR); // escritura del buffer de bwss al socket UDP
        
        // AGREGAR TIMEOUT Y HACER QUE ESPERE
        do {
        	t = clock();
        	while(!received_ack && (((double)(tEnd = clock() - t) / CLOCKS_PER_SEC) < timeout));

        	if(!received_ack) {
        		fprintf(stderr, "REENVIANDO: ");
        		DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR); 
        	}
        } while(!received_ack);

        received_ack = 0;
	}
	ack_bit = frame_seq; // ack del eof
	final_bwssbuff[DTYPE] = 'D';
    addNumSeq(frame_seq, final_bwssbuff);
	DwriteUDP(bwss_socket, final_bwssbuff, DHDR); // escritura de EOF al socket UDP
	do { // chequea que se recibio ack del eof por parte del bwss
    	t = clock();
    	while(!received_ack && (((double)(tEnd = clock() - t) / CLOCKS_PER_SEC) < timeout));

    	if(!received_ack) {
    		fprintf(stderr, "REENVIANDO: ");
    		DwriteUDP(bwss_socket, final_bwssbuff, DHDR); 
    	}
    } while(!received_ack);

	pthread_mutex_lock(&mutex);
	while(!ready) // Espera a que haya terminado proceso de retorno de los datos desde bws para finalizar
		pthread_cond_wait(&cond, &mutex);
	Dclose(bwc_socket); // cierre del socket TCP
	pthread_mutex_unlock(&mutex);
	fprintf(stderr, "SOCKET TCP CERRADO\n");
	return NULL;
}
