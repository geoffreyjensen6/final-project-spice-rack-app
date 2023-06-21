/*-----------------------------------------------------------------------------
Author: Geoffrey Jensen
ECEA 5307 Final Project (Leveraged work from ECEA5306 Assignment #6)
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


#define WRITE_FILE "/usr/bin/spice_rack/spice_rack_consolidated.txt"
#define PORT "9000"
#define BACKLOG 20
#define READ_WRITE_SIZE 1024

static bool caught_signal = false;

struct arg_struct {
	int connected_skt_fd;
	int thread_complete;
};

struct thread_info {
	pthread_t thread_id;
	struct arg_struct input_args;
};

struct slist_data_struct {
	struct thread_info tinfo;
	SLIST_ENTRY(slist_data_struct) entries;
};

SLIST_HEAD(slisthead,slist_data_struct) head = SLIST_HEAD_INITIALIZER(head);
pthread_mutex_t write_lock;

void read_file_and_send(int socket_fd, int reader_fd){
	ssize_t bytes_read, bytes_written;
	char write_buffer[READ_WRITE_SIZE] = "";
	while ((bytes_read = read(reader_fd,write_buffer,READ_WRITE_SIZE)) != 0){
		if(bytes_read == -1) {
			if (errno == EINTR){
				continue;
			}
			perror("aesdsocket_server: read_file_and_send - Read file error: ");
			syslog(LOG_DEBUG, "aesdsocket_server: read_file_and_send - Read file error: %s\n", strerror(errno));
			return;
		}
		while(1){
			bytes_written = write(socket_fd,write_buffer,bytes_read);
			printf("Sending...\n%s\n", write_buffer);
			if(bytes_written == -1){
				if (errno == EINTR){
					continue;
				}
				perror("aesdsocket_server: read_file_and_send - socket write error: ");
				syslog(LOG_DEBUG, "aesdsocket_server: read_file_and_send - socket write error: %s\n", strerror(errno));
				return;
			}
			else{
				break;
			}
		}
	}
	return;
}

static void socket_signal_handler (int signal_number){
	if (signal_number == SIGTERM || signal_number == SIGINT){
		caught_signal = true;
	} 
}

void *data_processor(void *input_args){
	struct arg_struct *in_args = input_args;
	int writer_fd = open(WRITE_FILE, O_RDONLY | O_CREAT | O_APPEND, 0644);
	if(writer_fd == -1){
		perror("aesdsocket_server: data_processor - Unable to open file: ");
		syslog(LOG_DEBUG, "aesdsocket_server: data_processor - Unable to open %s file: %s\n", WRITE_FILE, strerror(errno));
	}
	read_file_and_send(in_args->connected_skt_fd, writer_fd);
	in_args->thread_complete=1;
	close(writer_fd);
	return input_args;
}

int add_slist_entry(int connected_skt_fd){
	struct slist_data_struct *entry;
	struct slist_data_struct *current_entry;
	entry  = (struct slist_data_struct*)malloc(sizeof(struct slist_data_struct));
	entry->tinfo.input_args.connected_skt_fd = connected_skt_fd;
	entry->tinfo.input_args.thread_complete = 0;
	pthread_create(&entry->tinfo.thread_id,NULL,data_processor,(void *)&entry->tinfo.input_args);
	if(SLIST_EMPTY(&head) != 0){
		SLIST_INSERT_HEAD(&head, entry, entries);
	}
	else{
		SLIST_FOREACH(current_entry, &head, entries){
			if(current_entry->entries.sle_next == NULL){
				SLIST_INSERT_AFTER(current_entry, entry, entries);
				break;
			}
		}
	}
	return 0;
}

//Check the input argument count to ensure both arguments are provided
int main(int argc, char *argv[]){
	int skt_fd, connected_skt_fd, daemon_pid, ret_val;
	struct addrinfo skt_addrinfo, *res_skt_addrinfo, *rp;
	struct sockaddr connected_sktaddr;
        char client_ip_hostview[INET_ADDRSTRLEN];
	struct sigaction socket_sigaction;
	int yes=1;
	struct slist_data_struct *current_entry;

	openlog(NULL,0,LOG_USER);
	syslog(LOG_DEBUG,"aesdsocket_server main - Starting Script Over\n");	

	//Initialize SLIST Head
	SLIST_INIT(&head);

	//Setup signal handler
	memset(&socket_sigaction,0,sizeof(struct sigaction));
	socket_sigaction.sa_handler=socket_signal_handler;
	if(sigaction(SIGTERM, &socket_sigaction, NULL) != 0) {
		perror("aesdsocket_server: main - Error registering for SIGTERM - ");
                syslog(LOG_DEBUG, "aesdsocket_server: main - Error registering for SIGTERM - %s\n", strerror(errno));
	}
	if(sigaction(SIGINT, &socket_sigaction, NULL) != 0) {
		perror("aesdsocket_server: main - Error registering for SIGINT - ");
                syslog(LOG_DEBUG, "aesdsocket_server: main - Error registering for SIGINT - %s\n", strerror(errno));
	}

	//Setup addrinfo struct
	socklen_t sktaddr_size;
	memset(&skt_addrinfo,0,sizeof skt_addrinfo);
	skt_addrinfo.ai_family = AF_INET;
	skt_addrinfo.ai_socktype = SOCK_STREAM;
	skt_addrinfo.ai_flags = AI_PASSIVE;
	ret_val = getaddrinfo(NULL,PORT,&skt_addrinfo,&res_skt_addrinfo);
	if(ret_val != 0){
		perror("aesdsocket_server: main - getaddrinfo() failed - ");
		syslog(LOG_DEBUG,"aesdsocket_server: main - gettaddrinfo() returned %s\n",gai_strerror(ret_val));
		return -1;
	}

	//Open socket and Bind
	for(rp = res_skt_addrinfo; rp != NULL; rp = rp->ai_next){
		skt_fd = socket(PF_INET,SOCK_STREAM | SOCK_NONBLOCK,0);
		if(skt_fd == -1){
			perror("aesdsocket_server: main - socket() failed - ");
			syslog(LOG_DEBUG, "aesdsocket_server: main - socket() failed - %s\n", strerror(errno));
			continue;
		}	
		if(setsockopt(skt_fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes) == -1){
			perror("aesdsocket_server: main - setsockopt failed - ");
			syslog(LOG_DEBUG, "aesdsocket_server: main - setsockopt failed - %s\n", strerror(errno));
		}
		ret_val = bind(skt_fd,res_skt_addrinfo->ai_addr,res_skt_addrinfo->ai_addrlen);
		if(ret_val != 0){
			perror("aesdsocket_server: main - bind() failed - ");
			syslog(LOG_DEBUG,"aesdsocket_server: main - bind() failed - %s\n", strerror(errno));
			continue;
		}
		else{
			syslog(LOG_DEBUG,"aesdsocket_server: main - bind() successful\n");
			break;
		}
	}
	freeaddrinfo(res_skt_addrinfo);
	
	//Listen
	ret_val = listen(skt_fd,BACKLOG);
	if(ret_val != 0){
		perror("aesdsocket_server: main - listen() failed: ");
		syslog(LOG_DEBUG, "aesdsocket_server: main - listen() failed - %s\n", strerror(errno));
		return -1;
	}

	//Start Daemon if user provided -d argument
	if(argc == 2){
		if(strcmp(argv[1],"-d") == 0){
			syslog(LOG_DEBUG,"aesdsocket_server: main - Starting Daemon\n");
			//Create Daemon
			daemon_pid = fork();
			if (daemon_pid == -1){
				return -1;
			}
			else if (daemon_pid != 0){
				exit(EXIT_SUCCESS);
			}
			setsid();
			chdir("/");
			open("/dev/null",O_RDWR);
			dup(0);
			dup(0);
		}
	}

	while(1){
		//Establish Accepted Connection
		sktaddr_size = sizeof connected_sktaddr; 
		if(caught_signal == true){
			syslog(LOG_DEBUG, "aesdsocket_server: main - Caught signal, exiting\n");
			SLIST_FOREACH(current_entry, &head, entries){
				if(current_entry->tinfo.input_args.thread_complete == 1){
					pthread_join(current_entry->tinfo.thread_id,NULL);
				}
				//Close connection
				close(current_entry->tinfo.input_args.connected_skt_fd);
			}
			//Free all elements of SLIST
			struct slist_data_struct *tmp_slist_struct;
			while(SLIST_EMPTY(&head) != 0){
				tmp_slist_struct = SLIST_FIRST(&head);
				SLIST_REMOVE_HEAD(&head, entries);
				free(tmp_slist_struct);
			}
			close(skt_fd);
			//close(writer_fd);
			closelog();
			return 0;
		}
		connected_skt_fd = accept(skt_fd,&connected_sktaddr,&sktaddr_size); 
		if(connected_skt_fd == -1){
			if(errno == EAGAIN || errno == EWOULDBLOCK){
				continue;
			}
			perror("aesdsocket_server: main - accept() failed - ");
			syslog(LOG_DEBUG, "aesdsocket_server: main - accept() failed - %s\n", strerror(errno));
			return -1;
		}
		//Launch Thread and Create SLIST entry to store thread ID
		add_slist_entry(connected_skt_fd);
		//Check for Threads ready for joining
		SLIST_FOREACH(current_entry, &head, entries){
			if(current_entry->tinfo.input_args.thread_complete == 1){
				syslog(LOG_DEBUG,"aesdsocket_server: main - Attempting to join thread pointed to by %i\n",current_entry->tinfo.input_args.connected_skt_fd);
				pthread_join(current_entry->tinfo.thread_id,NULL);
				//Close connection
				close(current_entry->tinfo.input_args.connected_skt_fd);
				syslog(LOG_DEBUG,"aesdsocket_server: main - Closed connection from %s\n",client_ip_hostview);	
			}
			current_entry->tinfo.input_args.thread_complete = 0;
		}
	}
	return 0;
}
