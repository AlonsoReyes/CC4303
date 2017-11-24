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


char package_window[WIN_SZ][BUFFER_LENGTH + DHDR];
int cntArray[WIN_SZ];

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

int expectedSeqNum; // Indica el numero de secuencia del ACK que se espera
int frame_seq; // Indica el numero de secuencia del paquete que sigue
int last_sent; // Indica el numero de secuencia del último paquete enviado de la ventana
int free_space;
int timeout; // Es el timeout que se utilizara
int received_ack; // Indica si se recibe el ACK;
int fast_retransmit;

int LAR; // last ack received
int windFirst; // indica el ultimo paquete que se debe enviar de la ventana de emision
int duplicated; // indica cantidad de acks duplicados
int recentlyReceived; // ack recientemente recibido

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

    expectedSeqNum = 0; // Indica numero de secuencia del ACK enviado. Parte en el 0
    frame_seq = 0; // Indica numero de secuencia del paquete enviado. Parte en el 0
    last_sent = 0;
    windFirst = 0;
    timeout = 1; // Indica tiempo de espera por un paquete. 1 segundo
    received_ack = 0; // Indica si se recibio el ack del paquete enviado.
    duplicated = 0;
    LAR = -1;
    fast_retransmit = 0;

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
	int lastReceivedSeqNum = 0; // permite descartar archivos duplicados


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
	    	//printf("Received Seq Num: %d\n", receivedSeqNum);
	    	
	    	if(receivedSeqNum > frame_seq - 1){
	    		recentlyReceived = -1;
	    	}
	    	else{
	    		recentlyReceived = receivedSeqNum;
	    	}
	    	printf("Recently Seq Num: %d\n", recentlyReceived);
	    	printf("expectedSeqNum Seq Num: %d\n", expectedSeqNum);
	   
	    	if(receivedSeqNum == expectedSeqNum - 1) {
		    	duplicated++;
		    	if(duplicated == 3) {
		    		fast_retransmit = 1;
		    	}
		    } else if(recentlyReceived >= expectedSeqNum) {
	    		expectedSeqNum = recentlyReceived + 1;
	    		//LAR = recentlyReceived;
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
			 
			if(lastReceivedSeqNum == receivedSeqNum) { // Evita escribir a bwc paquetes repetidos
				DwriteUDP(bwss_socket, final_bwssbuff, DHDR);
				copyArray(DHDR, 0, cnt - DHDR, bwc_buffer, final_bwcbuff);
				Dwrite(bwc_socket, final_bwcbuff, cnt - DHDR); // escritura al socket TCP del cliente 
				lastReceivedSeqNum = (lastReceivedSeqNum + 1) % WIN_SZ;
			}
	    	//lastReceivedSeqNum  = receivedSeqNum;
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

int sendPartialWindows(int *pck, int *indexWindows){
	int cnt;
	printf("%s\n", "Send Partial");
	printf("Package: %d\n", *pck);
	printf("windFirst: %d\n", windFirst);
	while(*pck < WIN_SZ && !fast_retransmit) {
		cnt = Dread(bwc_socket, bwss_buffer, BUFFER_LENGTH); // lectura del socket bwc al buffer de bwss
	    if(cnt <= 0) break; // condicion de quiebre. Lectura de EOF

	    // DEBO MODIFICAR BUFFER PARA CONTENER HEADER
	    final_bwssbuff[DTYPE] = 'D';
	    addNumSeq(frame_seq, final_bwssbuff);
	    copyArray(0, DHDR, BUFFER_LENGTH + DHDR, bwss_buffer, final_bwssbuff);

	    printf("Enviando paquete: %d of type %c\n",  frame_seq, final_bwssbuff[DTYPE]);
	    frame_seq = (frame_seq + 1) % MAX_SEQ; // Proximo numero de secuencia a enviar

		strcpy(package_window[*indexWindows%WIN_SZ], final_bwssbuff); // guardo paquete en la ventana de emision
		printf("Package window: %d of type %c\n",  getNumSeq(package_window[*indexWindows%WIN_SZ]), package_window[*indexWindows%WIN_SZ][DTYPE]);
		cntArray[*indexWindows%WIN_SZ] = cnt + DHDR;
	    (*pck)++;
	    (*indexWindows)++;

	    // Mandamos el paquete de la ventana
	    DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR); // escritura del buffer de bwss al socket UDP
	}
	return cnt;
}

void sendFullWindows(int winSZ){
	printf("%s\n", "Send Full");
	printf("windFirst: %d\n", windFirst);
	for(int i = 0; i < winSZ; i++){
		package_window[(windFirst+i)%WIN_SZ][DTYPE] = 'D';
		printf("Sending package: %d of type %c\n", getNumSeq(package_window[(windFirst+i)%WIN_SZ]), package_window[(windFirst+i)%WIN_SZ][DTYPE]);
		DwriteUDP(bwss_socket,package_window[(windFirst+i)%WIN_SZ],cntArray[(windFirst+i)%WIN_SZ]);
	}
}

/*  Funcion encargada de realizar el proceso de lectura y escritura del paquete de retorno desde bwc a bwss  */
void* connect_client(void *pcl){
	int cnt, pck, indexWindows, actualizationNum;
	clock_t t, tEnd;
	bwc_socket = *((int *)pcl);
    free(pcl);

	DwriteUDP(bwss_socket, bwss_buffer, 0); // mensaje enviado para "confirmar" conexion con bwss
	
	pck = 0;
	indexWindows = 0;

	/* Proceso de lectura y escritura desde bwc a bwss */
	for(;;) {
		cnt = sendPartialWindows(&pck, &indexWindows);
		if(cnt == 0) break;
		// AGREGAR TIMEOUT Y HACER QUE ESPERE
		do {
		// VERIFICA SI ACK M == LAR => DUP += 1 => SI DUP == 3 => ENVIAR VENTANA COMPLETA
			t = clock();
			while(!received_ack && (((double)(tEnd = clock() - t) / CLOCKS_PER_SEC) < timeout) && !fast_retransmit);
			printf("DUPLICADOS ANTES: %d\n", duplicated);
			if(!received_ack || fast_retransmit) {
				fprintf(stderr, "REENVIANDO VENTANA: \n");
				sendFullWindows(WIN_SZ);
				if(fast_retransmit) {
					duplicated = 0;
					fast_retransmit = 0;
					received_ack = 0;
				}
				//DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR); 
			}
		} while(!received_ack);
	
        // RECIBI EL ACK M -> TENGO VENTANA - M ESPACIOS LIBRES
        
        if(LAR > recentlyReceived) {
        	actualizationNum = (MAX_SEQ - LAR + recentlyReceived +1);
        } else {
        	actualizationNum =  (recentlyReceived - LAR);
        }
        pck -= actualizationNum;
        windFirst += actualizationNum;
        windFirst %= WIN_SZ;
        //indexWindows++;
        LAR = recentlyReceived;
        duplicated = 0;
        received_ack = 0;
	}
	//expectedSeqNum = frame_seq; // ack del eof
	final_bwssbuff[DTYPE] = 'D';
    addNumSeq(frame_seq, final_bwssbuff);
    copyArray(0, DHDR, BUFFER_LENGTH + DHDR, bwss_buffer, final_bwssbuff);

	strcpy(package_window[indexWindows%WIN_SZ], final_bwssbuff); // guardo paquete en la ventana de emision
	cntArray[indexWindows%WIN_SZ] = cnt + DHDR;
	indexWindows++;
	frame_seq++;
    // Mandamos el paquete de la ventana
    DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR);

    while (!(expectedSeqNum - 1 == frame_seq - 1)) {
    	printf("LAR: %d - Frame Seq: %d\n", LAR, frame_seq);
		do {
		// VERIFICA SI ACK M == LAR => DUP += 1 => SI DUP == 3 => ENVIAR VENTANA COMPLETA
			t = clock();
			while(!received_ack && (((double)(tEnd = clock() - t) / CLOCKS_PER_SEC) < timeout) && !fast_retransmit);

			if(!received_ack || fast_retransmit) {
				fprintf(stderr, "REENVIANDO VENTANA: \n");
				if (indexWindows <= WIN_SZ) {
					sendFullWindows(indexWindows);
				} else {
					sendFullWindows(WIN_SZ);
				}
				if(fast_retransmit) {
					duplicated = 0;
					fast_retransmit = 0;
					received_ack = 0;
				}
			}
		} while(!received_ack);
		LAR = recentlyReceived;
        duplicated = 0;
        received_ack = 0;
	}

	pthread_mutex_lock(&mutex);
	while(!ready) // Espera a que haya terminado proceso de retorno de los datos desde bws para finalizar
		pthread_cond_wait(&cond, &mutex);
	Dclose(bwc_socket); // cierre del socket TCP
	pthread_mutex_unlock(&mutex);
	fprintf(stderr, "SOCKET TCP CERRADO\n");
	return NULL;
}
