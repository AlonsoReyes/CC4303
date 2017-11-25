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

/* totalmente necesarias */

pthread_t bwss_thread; // thread de TCP a UDP
pthread_t bwc_thread; // thread de UDP a TCP

pthread_mutex_t mutex;
pthread_cond_t cond;

/*comunicacion*/
char *bwss_port; // puerto de bwss
char *bwc_port; // puerto de bwc

int bwc_socket;
int bwss_socket;

/*buffers*/
char package_window[WIN_SZ][BUFFER_LENGTH + DHDR];
int cntArray[WIN_SZ];

char bwc_buffer[BUFFER_LENGTH + DHDR]; // buffer entre bwss y bwcs (UDP)
char bwss_buffer[BUFFER_LENGTH]; // buffer entre bwc y bwcs (TCP)
char final_bwssbuff[BUFFER_LENGTH + DHDR]; // buffer entre bwcs y bwss (UDP)
char final_bwcbuff[BUFFER_LENGTH]; // buffer entre bwcs y bwc (TCP)

/*indicadores globales*/
int ready; // indicara cuando se habra terminado la escritura hacia bwc (retorno del mensaje) para soltar el socket
int timeout; // Es el timeout que se utilizara
int received_ack; // Indica si se recibe un ACK;

int expected_seq_num; // Indica el numero de secuencia del ACK que se espera
int next_seq_num; // Indica el numero de secuencia del paquete que sigue


// LAR DEBE SER DE LOS QUE SE HAN ENVIADO
int last_ack_received; // numero de secuencia del ultimo paquete recibido
int last_frame_sent; // numero de secuencia del ultimo paquete enviado

int first_package_pos; // indica el primer paquete que se debe enviar de la ventana de emision
int first_package_sqnum;
/*indicadores para fast retransmit*/
int fast_retransmit_enabled; // Indica si se puede hacer fast retransmit
int duplicated; // indica cantidad de acks duplicados


/*declaracion de funciones*/
void *connect_client(void *pcl);
void *bwc_connect(void * ptr); 
void *bwss_connect(void *ptr);

/* ------------*/


int recentlyReceived; // ack recientemente recibido




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
int get_seq_num(char *buf) {
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

    expected_seq_num = 0; // Indica numero de secuencia del ACK enviado. Parte en el 0
    next_seq_num = 0; // Indica numero de secuencia del paquete enviado. Parte en el 0
    first_package_pos = 0;
    first_package_sqnum = 0;
    timeout = 1; // Indica tiempo de espera por un paquete. 1 segundo
    received_ack = 0; // Indica si se recibio el ack del paquete enviado.
    duplicated = 0;
    last_ack_received = -1;
    last_frame_sent = -1;
    fast_retransmit_enabled = 1;

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
	int bytes, cnt, size, received_seq_num, first, data_seq_num;

	first = 1;

	/* conexion del socket UDP al puerto de BWSS */
	if((bwss_socket = j_socket_udp_connect((char *)ptr, bwss_port)) < 0) {
		printf("udp connect failed\n");
       	exit(1);
	}

	/* Proceso de lectura y escritura desde socket UDP (bwss) a socket TCP (bwc) */
	for(bytes=0;; bytes+=cnt) {
		size = BUFFER_LENGTH + DHDR; // maximo a leer
	    cnt = read(bwss_socket, bwc_buffer, size); // lectura del socket UDP al buffer correspondiente al cliente

	    received_seq_num = get_seq_num(bwc_buffer);

	    if (received_seq_num == MAX_SEQ - 1) {
	    	received_seq_num = - 1;
	    }
	    if(bwc_buffer[0] == 'A') {
	    	printf("received_seq_num: %d - expected_seq_num: %d\n", received_seq_num, expected_seq_num);
	   		
	    	if(received_seq_num == expected_seq_num - 1) {
		    	if (fast_retransmit_enabled) {
		    		pthread_mutex_lock(&mutex);
		    		duplicated++;
		    		pthread_mutex_unlock(&mutex);
		    	}
		    } else if(received_seq_num%MAX_SEQ >= expected_seq_num) {
	    		expected_seq_num = (received_seq_num + 1)%MAX_SEQ;
	    		pthread_mutex_lock(&mutex);
	    		last_ack_received = received_seq_num;
	    		//printf("LAR: %d\n", last_ack_received);
		    	received_ack = 1;
		    	pthread_mutex_unlock(&mutex);
		    } 
	    } else if(bwc_buffer[0] == 'D') {

	    	printf("Data seq_num: %d - expected_seq_num: %d\n", received_seq_num, data_seq_num);
	    	if (first) {
	    		first = 0;
	    		data_seq_num = 0;
	    	}
	    	memset(final_bwssbuff, 0, BUFFER_LENGTH + DHDR);
	    	final_bwssbuff[0] = 'A';

	    	//mando antes a tcp
	    	// ARREGLAR
	    	if(cnt - DHDR <= 0 && data_seq_num == received_seq_num) { // identifica EOF de bwss
		  		printf("EOF Seq Num: %d\n", received_seq_num);
		  		break;
			}
			 
			if(data_seq_num == received_seq_num) { // Evita escribir a bwc paquetes repetidos
				addNumSeq(received_seq_num, final_bwssbuff);
				DwriteUDP(bwss_socket, final_bwssbuff, DHDR);
				copyArray(DHDR, 0, cnt - DHDR, bwc_buffer, final_bwcbuff);
				Dwrite(bwc_socket, final_bwcbuff, cnt - DHDR);	 // escritura al socket TCP del cliente 
				data_seq_num = (data_seq_num + 1)%MAX_SEQ;
			} else {
				addNumSeq(data_seq_num - 1, final_bwssbuff);
				DwriteUDP(bwss_socket, final_bwssbuff, DHDR);
			}
 	  	}
	    
	    //fprintf(stderr, "ReadUDP: %d bytes\n", cnt);
		
    }
    addNumSeq(received_seq_num, final_bwssbuff);
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
	int s, s2, *p;

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

int send_partial_window(int *pck, int *indexWindows){
	int cnt;
	cnt = -1;
	printf("%s\n", "Send Partial");
	//printf("Package: %d\n", *pck);
	//printf("first_package_pos: %d\n", first_package_pos);
	while(*pck < WIN_SZ && !(fast_retransmit_enabled && duplicated >=3)) {
		cnt = Dread(bwc_socket, bwss_buffer, BUFFER_LENGTH); // lectura del socket bwc al buffer de bwss
	    if(cnt <= 0) break; // condicion de quiebre. Lectura de EOF

	    // DEBO MODIFICAR BUFFER PARA CONTENER HEADER
	    final_bwssbuff[DTYPE] = 'D';
	    addNumSeq(next_seq_num, final_bwssbuff);
	    copyArray(0, DHDR, BUFFER_LENGTH + DHDR, bwss_buffer, final_bwssbuff);

	    last_frame_sent = next_seq_num;

	    //printf("Enviando paquete: %d of type %c\n",  next_seq_num, final_bwssbuff[DTYPE]);
	    next_seq_num = (next_seq_num + 1) % MAX_SEQ; // Proximo numero de secuencia a enviar

	    copyArray(0, 0, BUFFER_LENGTH + DHDR, final_bwssbuff, package_window[*indexWindows%WIN_SZ]);

		//strcpy(package_window[*indexWindows%WIN_SZ], final_bwssbuff); // guardo paquete en la ventana de emision
		//printf("Package window: %d of type %c\n",  get_seq_num(package_window[*indexWindows%WIN_SZ]), package_window[*indexWindows%WIN_SZ][DTYPE]);
		cntArray[*indexWindows%WIN_SZ] = cnt + DHDR;
	    (*pck)++;
	    (*indexWindows)++;

	    // Mandamos el paquete de la ventana
	    DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR); // escritura del buffer de bwss al socket UDP
	}
	fast_retransmit_enabled = 1;
	return cnt;
}

void send_full_window(int winSZ){
	printf("%s\n", "Send Full");
	//printf("last_frame_sent: %d - first_package_sqnum: %d\n", last_frame_sent ,first_package_sqnum);
	for(int i = 0; i < winSZ; i++){
		package_window[(first_package_pos+i)%WIN_SZ][DTYPE] = 'D';
		//printf("Sending package: %d of type %c\n", get_seq_num(package_window[(first_package_pos+i)%WIN_SZ]), package_window[(first_package_pos+i)%WIN_SZ][DTYPE]);
		DwriteUDP(bwss_socket,package_window[(first_package_pos+i)%WIN_SZ],cntArray[(first_package_pos+i)%WIN_SZ]);
	}
}

/*  Funcion encargada de realizar el proceso de lectura y escritura del paquete de retorno desde bwc a bwss  */
void* connect_client(void *pcl){
	int cnt, pck, indexWindows, actualizationNum, winSZ, actualizationPackageNum;
	clock_t t, tEnd;
	bwc_socket = *((int *)pcl);
    free(pcl);

	DwriteUDP(bwss_socket, bwss_buffer, 0); // mensaje enviado para "confirmar" conexion con bwss
	
	pck = 0;
	indexWindows = 0;
	actualizationPackageNum = 0;

	/* Proceso de lectura y escritura desde bwc a bwss */
	for(;;) {
		cnt = send_partial_window(&pck, &indexWindows);
		if(actualizationPackageNum){
			first_package_sqnum = get_seq_num(package_window[first_package_pos]);
			actualizationPackageNum = 0;
		}
		if(cnt == 0) break;
		// AGREGAR TIMEOUT Y HACER QUE ESPERE
		do {
		// VERIFICA SI ACK M == last_ack_received => DUP += 1 => SI DUP == 3 => ENVIAR VENTANA COMPLETA
			t = clock();
			while(!received_ack && (((double)(tEnd = clock() - t) / CLOCKS_PER_SEC) < timeout) 
				&& !(fast_retransmit_enabled && duplicated >= 3));			
			winSZ = last_frame_sent - first_package_sqnum + 1;
			if (fast_retransmit_enabled && duplicated>=3) {
				fprintf(stderr, "FAST RETRANSMIT DUP = %d\n", duplicated);
				duplicated = 0;
				fast_retransmit_enabled = 0;
				send_full_window(winSZ);
				// receive_ack = 0 ??
			} else if((tEnd / CLOCKS_PER_SEC) >= timeout) {
				fprintf(stderr, "DELAY RETRANSMIT \n");
				duplicated = 0;
				send_full_window(winSZ);
				//DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR); 
			}
		} while(!received_ack);
	
        // RECIBI EL ACK M -> TENGO VENTANA - M ESPACIOS LIBRES
        pthread_mutex_lock(&mutex);
        if(last_ack_received == last_frame_sent) {
        	pck = 0;
        	first_package_pos = 0;
        	actualizationPackageNum = 1;
        	//fprintf(stderr, "ACT POST\n");
        	//fprintf(stderr, "AQUIIIIIIIIIIIII, FPP: %d, FPS: %d, LAR: %d\n",first_package_pos,first_package_sqnum,last_ack_received);
        }
        else{
	        actualizationNum = (last_ack_received - first_package_sqnum + 1)%MAX_SEQ;
	        pck -= actualizationNum;
	        first_package_pos = (first_package_pos + actualizationNum)%WIN_SZ;
	        //fprintf(stderr, "AQUIIIIIIIIIIIII, FPP: %d, FPS: %d, LAR: %d\n",first_package_pos,first_package_sqnum,last_ack_received);
	        first_package_sqnum = get_seq_num(package_window[first_package_pos]);
	        //fprintf(stderr, "ACAAAAAAAAAAAAAA\n");
	    }
	    if (fast_retransmit_enabled) {
        	duplicated = 0;
        }
		received_ack = 0;
	    pthread_mutex_unlock(&mutex);
        //indexWindows++;
	}

	final_bwssbuff[DTYPE] = 'D';
    addNumSeq(next_seq_num, final_bwssbuff);
    copyArray(0, DHDR, BUFFER_LENGTH + DHDR, bwss_buffer, final_bwssbuff);

    last_frame_sent = next_seq_num;

	strcpy(package_window[indexWindows%WIN_SZ], final_bwssbuff); // guardo paquete en la ventana de emision
	cntArray[indexWindows%WIN_SZ] = cnt + DHDR;
	indexWindows++;
	next_seq_num++;
	//printf("LAST PACKAGE SEQ NUM %d\n", next_seq_num - 1);
    // Mandamos el paquete de la ventana
    DwriteUDP(bwss_socket, final_bwssbuff, cnt + DHDR);

    while (last_ack_received != last_frame_sent) {
    	//printf("last_ack_received: %d - Frame Seq: %d\n", last_ack_received, next_seq_num);
		do {
		// VERIFICA SI ACK M == last_ack_received => DUP += 1 => SI DUP == 3 => ENVIAR VENTANA COMPLETA
			t = clock();

			while(!received_ack && (((double)(tEnd = clock() - t) / CLOCKS_PER_SEC) < timeout) 
				&& !(fast_retransmit_enabled && duplicated >= 3));

			winSZ = last_frame_sent - first_package_sqnum + 1;
			if (fast_retransmit_enabled && duplicated>=3) {
				fprintf(stderr, "FAST RETRANSMIT DUP = %d\n", duplicated);
				duplicated = 0;
				fast_retransmit_enabled = 0;
				send_full_window(winSZ);
				// receive_ack = 0 ??
			} else if((tEnd / CLOCKS_PER_SEC) >= timeout) {
				fprintf(stderr, "DELAY RETRANSMIT \n");
				duplicated = 0;
				send_full_window(winSZ);
			}
		} while(!received_ack);

        if (fast_retransmit_enabled) {
        	duplicated = 0;
        }
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
