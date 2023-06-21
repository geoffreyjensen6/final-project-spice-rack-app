/*-----------------------------------------------------------------------------
Author: Geoffrey Jensen
ECEA 5307 Final Project
Date: 06/18/2023
-----------------------------------------------------------------------------*/

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <regex.h>


#define WRITE_FILE "/var/tmp/spices.txt"
#define BACKLOG 20
#define READ_WRITE_SIZE 1024

static bool caught_signal = false;

static int receive_and_write_to_file(int socket_fd, int writer_fd){
        ssize_t bytes_read, bytes_written;
        char *read_buffer;
       	int end_of_packet = 0;
	char newline = '\n';
	int result = 0;

	if((read_buffer = (char *)malloc(READ_WRITE_SIZE * sizeof(char))) == NULL){
		perror("aesdsocket_client: receive_and_write_to_file - Failed to Malloc - ");
		syslog(LOG_DEBUG, "aesdsocket_client: receive_and_write_to_file - Failed to Malloc - %s\n", strerror(errno));
	}
	memset(read_buffer, 0, READ_WRITE_SIZE);

        while(end_of_packet == 0){
                if((bytes_read = recv(socket_fd,read_buffer,READ_WRITE_SIZE,0)) != 0){
                        if(bytes_read == -1) {
                                if (errno == EINTR){
                                        continue;
                                }
                                perror("aesdsocket_client: receive_and_write_to_file - Error while trying to recv: ");
				syslog(LOG_DEBUG, "aesdsocket_client: receive_and_write_to_file - Error while trying to recv: %s", strerror(errno));
				result = -1;
                                break;
                        }
                        if(strchr(read_buffer,newline) != NULL){
                                end_of_packet = 1;
                        }
                        while(1){
                                bytes_written = write(writer_fd, read_buffer, bytes_read);
                                if(bytes_written == -1){
					if (errno == EINTR){
						continue;
					}
					perror("aesdsocket_client: receive_and_write_to_file - Error while trying to write to file: ");
					syslog(LOG_DEBUG, "aesdsocket_client: receive_and_write_to_file - Error while trying to write to file: %s", strerror(errno));
					result = -1;
					break;
				}
				else{
					break;
				}
                        }
                }

        }

	free(read_buffer);
	return result;
}


static void socket_signal_handler (int signal_number){
	if (signal_number == SIGTERM || signal_number == SIGINT){
		caught_signal = true;
	} 
}

int main(int argc, char *argv[]){
	int skt_fd, ret_val;
	char ip_address[16];
	char port[5];
	struct addrinfo skt_addrinfo, *res_skt_addrinfo, *rp;
	struct sigaction socket_sigaction;

	if(argc != 3){
		printf("Incorrect number of arguments were supplied. Use following syntax: aesdsocket_client <ip_address> <port>\n");
		return -1;
	}
	if((strlen(argv[1]) > 15) || (strlen(argv[2]) > 4)){
		printf("Arguments provided are longer than xxx.xxx.xxx.xxx for IP addr and xxxx for port num\n");
		return -1;
	}
	strcpy(ip_address, argv[1]);
	strcpy(port, argv[2]);

	openlog(NULL,0,LOG_USER);
	syslog(LOG_DEBUG,"aesdsocket_client: main - Starting Script Over\n");	

	//Setup signal handler
	memset(&socket_sigaction,0,sizeof(struct sigaction));
	socket_sigaction.sa_handler=socket_signal_handler;
	if(sigaction(SIGTERM, &socket_sigaction, NULL) != 0) {
		perror("aesdsocket_client: main - Error registering for SIGTERM - ");
		syslog(LOG_DEBUG, "aesdsocket_client: main - Error registering for SIGTERM - %s\n", strerror(errno));
	}
	if(sigaction(SIGINT, &socket_sigaction, NULL) != 0) {
		perror("aesdsocket_client: main - Error registering for SIGINT - ");
		syslog(LOG_DEBUG, "aesdsocket_client: main - Error registering for SIGINT - %s\n", strerror(errno));
	}

	//Setup addrinfo struct
	memset(&skt_addrinfo, 0, sizeof skt_addrinfo);
	skt_addrinfo.ai_family = AF_INET;
	skt_addrinfo.ai_socktype = SOCK_STREAM;
	skt_addrinfo.ai_flags = AI_PASSIVE;
	ret_val = getaddrinfo(ip_address,port,&skt_addrinfo,&res_skt_addrinfo);
	if(ret_val != 0){
		perror("aesdsocket_client: main - getaddrinfo failed - ");
		syslog(LOG_DEBUG,"aesdsocket_client: main - gettaddrinfo failed - %s\n", gai_strerror(ret_val));
		return -1;
	}

	//Open socket and Connect
	for(rp = res_skt_addrinfo; rp != NULL; rp = rp->ai_next){
		skt_fd = socket(rp->ai_family,SOCK_STREAM | SOCK_NONBLOCK,0);
		if(skt_fd == -1){
			perror("aesdsocket_client: main - socket() failed - ");
			syslog(LOG_DEBUG, "aesdsocket_client: main - socket() failed - %s\n", strerror(errno));
			continue;
		}	
		
		//Connect
		if(connect(skt_fd, rp->ai_addr, rp->ai_addrlen) == -1){
			syslog(LOG_DEBUG,"aesdsocket_client: main - connect() failed - %s\n", strerror(errno));
			continue;
		}
		else{
			syslog(LOG_DEBUG,"aesdsocket_client: main - connect() successful");
			break;
		}
	}
	printf("Connection Established. Will now read data in to %s\n", WRITE_FILE); 

	freeaddrinfo(res_skt_addrinfo);
	int writer_fd = creat(WRITE_FILE, 0644);

	//Give some time for socket to get setup and ready
	sleep(1);	

	while(receive_and_write_to_file(skt_fd, writer_fd) != 0){
		if(caught_signal == true){
			syslog(LOG_DEBUG, "aesdsocket_client: main - Caught signal, exiting");
			close(skt_fd);
			close(writer_fd);
			closelog();
			return 0;
		}
	}
	printf("Done writing data into %s. Closing Connection and exiting\n", WRITE_FILE);
	close(skt_fd);
	close(writer_fd);
	closelog();

	return 0;
}
