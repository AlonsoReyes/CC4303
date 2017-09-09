#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include "jsocket6.4.h"
#include "Data.h"
#include <pthread.h>

char bwc_buffer[BUFFER_LENGTH];
char bwss_buffer[BUFFER_LENGTH];

pthread_t bwss_thread;
pthread_t bwc_thread;

char *bwss_port = "2000";
char *bwc_port = "2001";

int bwc_socket;
int bwss_socket;
int ready;


pthread_mutex_t mutex;
pthread_cond_t cond;

void* connect_client(void *pcl);
void *bwc_connect(void * ptr); 
void *bwss_connect(void *ptr);


int DreadUDP(int cl, char *buf, int l) {
	int cnt, pos;
	int size = l;
    pos = 0;
    while(size > 0) {
        cnt = read(cl, buf+pos, size);
    if(cnt <= 0) return cnt;
    size -= cnt;
    pos += cnt;
    }
    fprintf(stderr, "DreadUDP: %d bytes\n", pos);
    return pos;
}

void DwriteUDP(int cl, char *buf, int l) {
	//fprintf(stderr, "BUFFERUDP:\n %s\n",buf);
    if(l >= 0) {
	    if(write(cl, buf, l) != l) {
	        perror("falló write en el socket");
	        exit(1);
	    }
    }
	fprintf(stderr, "DwriteUDP: %d bytes \n", l);
}


int main(int argc, char **argv) {
    char *bwss_server;

    if(argc == 1) {
		bwss_server = "localhost";
    } else if (argc == 2) {
    	bwss_server = argv[1];
    } else {
		fprintf(stderr, "Use: bwcs client_port bwss_server server_port\n");
		return 1;
    }

    ready = 0;
    pthread_mutex_init(&mutex,NULL);
    pthread_cond_init(&cond,NULL);

    pthread_create(&bwc_thread, NULL, bwc_connect, (void *)bwc_port);  
    pthread_create(&bwss_thread, NULL, bwss_connect, (void *)bwss_server);
    pthread_join(bwss_thread, NULL);
    pthread_join(bwc_thread, NULL);
}

void *bwss_connect(void *ptr) {
	int bytes, cnt;

	if((bwss_socket = j_socket_udp_connect((char *)ptr, bwss_port)) < 0) {
		printf("udp connect failed\n");
       	exit(1);
	}

	
	for(bytes=0;; bytes+=cnt) {
		cnt = DreadUDP(bwss_socket, bwc_buffer, BUFFER_LENGTH);
		Dwrite(bwc_socket, bwc_buffer, cnt);
		
        if(cnt <= 0) break;
    }

    pthread_mutex_lock(&mutex);
	ready = 1;
	Dclose(bwss_socket);
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
	return NULL;
}

void *bwc_connect(void* ptr) {
	Dbind(connect_client, (char *)ptr);
	return NULL;
}

void* connect_client(void *pcl){
	int bytes, cnt;
	int first = 1;
	bwc_socket = *((int *)pcl);

    free(pcl);
	for(bytes=0;; bytes+=cnt) {
		if(first) {
			DwriteUDP(bwss_socket, bwss_buffer, 0);
			first = 0;
		}
        cnt = Dread(bwc_socket, bwss_buffer, BUFFER_LENGTH);
        DwriteUDP(bwss_socket, bwss_buffer, cnt);
        if(cnt <= 0) break;
	}

	Dwrite(bwss_socket, bwss_buffer, 0);

	pthread_mutex_lock(&mutex);
	while(!ready)
		pthread_cond_wait(&cond, &mutex);
	Dclose(bwc_socket);
	pthread_mutex_unlock(&mutex);
	return NULL;
}