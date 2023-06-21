#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include "spice_rack_app.h"
#include <stdbool.h>

//Variables
#define MAX_FILE_ENTRY_LEN 32
#define MAX_LINE_LENGTH 160 //80 + (5*MAX_FILE_ENTRY)
#define SPICE_RACK_SIZE 3
#define NUM_COLUMNS 5
#define ADC_COLUMN 2
#define MASS_COLUMN 3
#define TSP_COLUMN 4
#define EMPTY_JAR_MASS_DEF 133.245
#define CALIBRATE_GPIO "27"
//Files
#define HX711_FILE "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define FSR_FILE "/dev/fsr_gpio_0"
#define OUTPUT_FILE "/usr/bin/spice_rack/spice_rack_measurements.txt"
#define CONSOLIDATED_FILE "/usr/bin/spice_rack/spice_rack_consolidated.txt"
#define TMP_FILE "/var/log/spice_rack_tmp.txt"
#define SPICE_CONVERSIONS_FILE "/usr/bin/spice_rack/spice_conversions.csv"

static struct spice_rack *spice_rack;
static struct calibration_status calibration;
static bool caught_signal = false;

static void socket_signal_handler (int signal_number){
        if (signal_number == SIGTERM || signal_number == SIGINT){
                caught_signal = true;
        }
}

//Finding the file size in order to malloc appropriately sized char *.
static off_t find_file_size(char *file_name){
	int fd = 0;
	off_t file_length = 0;

	fd = open(file_name, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: find_file_size - Failed to Open File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: find_file_size - Failed to Open File - %s\n", strerror(errno));
		return -1;
	}

	file_length = lseek(fd, 0, SEEK_END);
	if(file_length  == -1){
		perror("Spice_Rack_App: find_file_size - Seeking to end of file failed - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: find_file_size - Seeking to end of file failed - %s\n", strerror(errno));
		return -1;
	}
	syslog(LOG_DEBUG, "Spice_Rack_App: find_file_size - file_length = %li\n", file_length);

	close(fd);
	return file_length;
}

static int copy_file(int in_fd, int out_fd, off_t end_location){
	int result = 0;
	off_t rd_count = 0;
	off_t wr_count = 0;
	off_t read_len = 1;
	off_t curr_position;
	char next_char[1];
	
	//Grab current position because this function is only intended to copy file contents from current location to
	//a specified end location
	curr_position = lseek(in_fd, 0, SEEK_CUR);
	if(curr_position  == -1){
		perror("Spice_Rack_App: copy_file - Grabbing current position in file failed - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: copy_file - Grabbing current position in file - %s\n", strerror(errno));
		return -1;
	}

	//Check end location to determine how long to copy
	if(end_location != 0){
		read_len = end_location - curr_position;
	}
	else{
		read_len = 1;
	}
	
	//Copy contents
	while((rd_count = read(in_fd, next_char, 1)) != 0 && (read_len != 0)){
		if(rd_count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: copy_file -  Reading File failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: copy_file - Reading File failed - %s\n", strerror(errno));
			result = -1;
			break;
		}
		if(*next_char == EOF){
			break;
		}
		read_len = read_len - rd_count;
		wr_count = write(out_fd, next_char, 1);
		if(wr_count == -1){
			perror("Spice_Rack_App: copy_file - Writing Measurements to File failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: copy_file - Writing Measurements to File failed - %s\n", strerror(errno));
			result = -1;
		}
	}
		
	return result;
}

static int read_line(int fd, char *output_str){
	int count;
	int result = 0;
	char *read_buff;

	if((read_buff = (char *)malloc(2 * sizeof(char))) == NULL){
		printf("Spice_Rack_App: read_line - Failed on Malloc\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: read_line - Failed on Malloc\n");
		return -1;
	}
	memset(output_str,0,strlen(output_str));

	while((count = read(fd, read_buff, 1)) != 0){
		if(count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: read_line - Reading File failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: read_line - Reading File failed - %s\n", strerror(errno));
			result = -1;
			break;
		}
		if(strchr(read_buff, '\n') != NULL){
			break;
		}
		strncat(output_str, read_buff, 1);
	}
	
	free(read_buff);
	return result;
}

static int parse_line(char *output_str, int i){
	int result = 0;
	int substring_len = 0;
	char *start_ptr = output_str;
	char *comma_ptr;
	int j;

	for(j=0;j<NUM_COLUMNS;j++){
		if((start_ptr = strstr(output_str, spice_rack->spices[i].spice_search_strings.search_strings[j])) != NULL){
			start_ptr = start_ptr + strlen(spice_rack->spices[i].spice_search_strings.search_strings[j]);
		}
		if((comma_ptr = strchr(start_ptr, ',')) != NULL || (j == NUM_COLUMNS-1)){
			substring_len = comma_ptr - start_ptr + 1;
			snprintf(spice_rack->spices[i].spice_entries.entries[j], substring_len, "%s", start_ptr);
			start_ptr = output_str + strlen(spice_rack->spices[i].spice_search_strings.search_strings[j]) + substring_len;
		}
	}
	return result;
}

static off_t search_file(int in_fd, char *search_term){
	off_t file_offset;
	off_t end_of_line = 0;
	off_t end_of_file = 0;
	char read_line_buff[MAX_LINE_LENGTH];
	char *match_str;
	
	//Find EOF
	end_of_file = lseek(in_fd, 0, SEEK_END);
	if(end_of_file  == -1){
		perror("Spice_Rack_App: search_file - Seeking to end of file failed - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: search_file - Seeking to end of file failed - %s\n", strerror(errno));
		return -1;
	}

	//Set position to beginning of file
	file_offset = lseek(in_fd, 0, SEEK_SET);
	if(file_offset  == -1){
		perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: search_file - Seeking to beginning of file failed - %s\n", strerror(errno));
		return -1;
	}
	
	//Search through each line for the search term and stop on the line that you find it
	while(file_offset < end_of_file){	
		if(read_line(in_fd, read_line_buff) == -1){
			printf("Spice_Rack_App: search_file - Failure in reading line\n");
			syslog(LOG_DEBUG, "Spice_Rack_App: search_file - Failure in reading line\n");
			return -1;
		}
		if((match_str = strstr(read_line_buff, search_term)) != NULL){
			//Grab end of line position after the read is complete
			end_of_line = lseek(in_fd, 0, SEEK_CUR);
			if(end_of_line  == -1){
				perror("Spice_Rack_App: search_file - Grabbing end of line position failed - ");
				syslog(LOG_DEBUG, "Spice_Rack_App: search_file - Grabbing enf of line position failed - %s\n", strerror(errno));
				return -1;
			}
			
			//Restore file offset to beginning of this line
			file_offset = lseek(in_fd, file_offset, SEEK_SET);
			if(file_offset  == -1){
				perror("Spice_Rack_App: search_file - Seeking to beginning of line failed - ");
				syslog(LOG_DEBUG, "Spice_Rack_App: search_file - Seeking to beginning of line failed - %s\n", strerror(errno));
				return -1;
			}
			break;
		}		
		//Grab file offset of current location to determine if EOF
		file_offset = lseek(in_fd, 0, SEEK_CUR);
		if(file_offset  == -1){
			perror("Spice_Rack_App: search_file - Getting current file position failed  - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: search_file - Getting current file position failed - %s\n", strerror(errno));
			return -1;
		}
	}	

	return end_of_line;
}

static float convert_grams_to_tsp(char *spice_name, float grams){
	float result = 0;
	off_t spice_offset = 0;
	char output_str[50];
	char *comma_ptr;
	char res_str[10];
	int res_str_len;
	int fd;
	int i;
	char *end_ptr;
	float tsps;
	float ounces;

	fd = open(SPICE_CONVERSIONS_FILE, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: convert_grams_to_tsp - Failed to Open Conversions File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: convert_grams_to_tsp - Failed to Open Conversions File - %s\n", strerror(errno));
		return -1;
	}

	//Search for Spice Name in conversions file
	if((spice_offset = search_file(fd, spice_name)) == -1){
		printf("Spice_Rack_App: convert_grams_to_tsp - Searching for spice name in file observed an issue\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: convert_grams_to_tsp - Searching for spice name in file observed an issue\n");
		return -1;
	}
	else if(spice_offset == 0){
		printf("Couldn't find Entered Spice Name in %s\n", SPICE_CONVERSIONS_FILE);
		syslog(LOG_DEBUG, "Spice_Rack_App: convert_grams_to_tsp - Couldn't find Entered Spice Name in %s\n", SPICE_CONVERSIONS_FILE);
		return -1;
	}

	//Parse the line that contains the match to obtain the Grams to TSP conversion
	read_line(fd, output_str);
	for(i=0;i<3;i++){
		if((comma_ptr = strrchr(output_str, ',')) != NULL){
			comma_ptr = comma_ptr + 1;
			res_str_len = strlen(output_str) - (comma_ptr - output_str) + 1;
			snprintf(res_str, res_str_len, "%s", comma_ptr);
		}
	}

	//Do math
	//1gram = 0.03527grams
	syslog(LOG_DEBUG, "Spice_Rack_App: convert_grams_to_tsp - Grams is %f\n", grams);
	ounces = grams * 0.0352739619;
	syslog(LOG_DEBUG, "Spice_Rack_App: convert_grams_to_tsp - Ounces is %f\n", ounces);
	tsps = strtof(res_str, &end_ptr);
	syslog(LOG_DEBUG, "Spice_Rack_App: convert_grams_to_tsp - tsps is %f\n", tsps);
	result = tsps * ounces;
	syslog(LOG_DEBUG, "Spice_Rack_App: convert_grams_to_tsp - Total tsps is %f\n", result);
	
	close(fd);

	return result;
}

static int print_spice_list(){
	int fd;
	int i;
	off_t file_offset;
	off_t end_of_file;
	char output_str[50];
	char *comma_ptr;
	int res_str_len;
	char res_str[MAX_FILE_ENTRY_LEN];


	fd = open(SPICE_CONVERSIONS_FILE, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: print_spice_list - Failed to Open Consolidated File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: print_spice_list - Failed to Open Consolidated File - %s", strerror(errno));
		return -1;
	}

	//Find EOF
	end_of_file = lseek(fd, 0, SEEK_END);
	if(end_of_file  == -1){
		perror("Spice_Rack_App: search_file - Seeking to end of file failed - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: search_file - Seeking to end of file failed - %s\n", strerror(errno));
		return -1;
	}

	//Set position to beginning of file
	file_offset = lseek(fd, 0, SEEK_SET);
	if(file_offset  == -1){
		perror("Spice_Rack_App: print_spice_list - Seeking to beginning of file failed - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: print_spice_list - Seeking to beginning of file failed - %s\n", strerror(errno));
		return -1;
	}

	printf("Here are the list of spices found in %s\n", SPICE_CONVERSIONS_FILE);
	//Read Entire File
	while(file_offset < end_of_file){			
		read_line(fd, output_str);
		for(i=0;i<3;i++){
			if((comma_ptr = strchr(output_str, ',')) != NULL){
				res_str_len = comma_ptr - output_str + 1;
				snprintf(res_str, res_str_len, "%s", output_str);
			}
		}
		printf("%s\n", res_str);
		//Grab file offset of current location to determine if EOF
		file_offset = lseek(fd, 0, SEEK_CUR);
		if(file_offset  == -1){
			perror("Spice_Rack_App: print_spice_list - Getting current line position failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: print_spice_list - Getting current line position failed - %s\n", strerror(errno));
			return -1;
		}
	}

	close(fd);
	return 0;
}

//Used to store measurement data to a file. Handles both creating for the first time as well as updating 
//data for individual spices. 
static int store_measurement(int spice_num, char *spice_name, char *weight, float mass, float tsps){
	int input_fd;
	int temp_fd;
	int output_fd;
	int output_str_len;
	int count;
	int result = 0;
	off_t file_length = 0;
	off_t file_offset = 0;
	off_t eol;
	off_t match_offset = 0;
	char *spice_num_str;
	char file_name[] = OUTPUT_FILE;
	char *output_format_str;
	
	
	//Open file for RD/WR and create if it doesn't already exist. 
	input_fd = open(file_name, O_CREAT | O_RDWR);
	if(input_fd == -1){
		perror("Spice_Rack_App: store_measurements - Failed to Open Input File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurements - Failed to Open Input File - %s\n", strerror(errno));
		return -1;
	}
	temp_fd = open(TMP_FILE, O_CREAT | O_RDWR | O_TRUNC, 0666);
	if(temp_fd == -1){
		perror("Spice_Rack_App: store_measurements - Failed to Open File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurements - Failed to Open Temp File - %s\n", strerror(errno));
		return -1;
	}
	
	//Find File Size of Input File
	file_length = find_file_size(file_name);
	syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Input file length is %li\n", file_length);

	if(copy_file(input_fd, temp_fd, EOF) == -1){
		printf("Spice_Rack_App: store_measurement - Failed to read file contents to read buffer");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurements - Failed to Copy Input File to Temp file\n");
		return -1;
	}

	//Close input file now that contents were all read out. Going to repoen this file and truncate 
	//because often measurement changes could be fewer characters and lead to extra whitespace
	//and newlines at the EOF. So it is needed to write whole file again to avoid that.
	close(temp_fd);
	close(input_fd);
	temp_fd = open(TMP_FILE, O_RDONLY);
	if(temp_fd == -1){
		perror("Spice_Rack_App: store_measurements - Failed to Open File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurements - Failed to Open Temp File - %s\n", strerror(errno));
		return -1;
	}
	//Open file for writing and create if it doesn't already exist and truncate because we will rewrite
	output_fd = open(file_name, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if(output_fd == -1){
		perror("Spice_Rack_App: store_measurement - Failed to Open Output File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurements - Failed to Open Output File - %s\n", strerror(errno));
		return -1;
	}			
	
	//Generate the new output string
	//Output format is: Spice_Location, Spice_Name, ADC_Reading, Mass, Teaspoons
	//Setting the Spice_Location portion of the output string
	if((spice_num_str = (char *)malloc(MAX_FILE_ENTRY_LEN * sizeof(char))) == NULL){
		perror("Spice_Rack_App: store_measurement - Couldn't allocate memory - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Couldn't allocate memory - %s", strerror(errno));
		return -1;
	}
	memset(spice_num_str, 0, MAX_FILE_ENTRY_LEN);
	if((strstr(spice_name, "Empty Jar") != NULL) || (strcmp(spice_name, "Empty Rack") == 0)){
		snprintf(spice_num_str, MAX_FILE_ENTRY_LEN, "N/A-%s", spice_name);
	} 
	else{
		snprintf(spice_num_str,7,"Spice%i", spice_num);
	}

	//Setting the remaining portions of the output string
	output_str_len = MAX_LINE_LENGTH;
	output_format_str = (char *)malloc(output_str_len * sizeof(char));
	if(output_format_str == NULL){
		perror("Spice_Rack_App: store_measurement - Couldn't allocate memory - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Couldn't allocate memory - %s", strerror(errno));
		return -1;
	}
	memset(output_format_str, 0, output_str_len);
	snprintf(output_format_str, (output_str_len-1), "Spice_Location:%s,Spice_Name:%s,ADC_Reading:%s,Calibrated_Mass(grams):%3.6f,Teaspoons:%3.6f\n", spice_num_str, spice_name, weight, mass, tsps);

	if((eol = search_file(temp_fd, spice_num_str)) == -1){
		printf("Spice_Rack_App: store_measurement - Searching for spice location in file observed an issue\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Searching for spice location in file observed an issue\n");
		return -1;
	}
	else if(eol == 0){
		//Will be appending data to EOF
		//Reset position to beginning of file
		file_offset = lseek(temp_fd, 0, SEEK_SET);
		if(file_offset  == -1){
			perror("Spice_Rack_App: store_measurement - Seeking to beginning of file failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Seeking to beginning of file failed - %s\n", strerror(errno));
			result = -1;
		}

		//copy existing file contents
		if(copy_file(temp_fd, output_fd, EOF) == -1){
			perror("Spice_Rack_App: store_measurement - Failed to copy temp file contents to output file - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Failed to copy temp file contents to output file - %s\n", strerror(errno));
			result = -1;
		}

		//Write new Entry
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Output String = %s", output_format_str);
		count = write(output_fd, output_format_str, strlen(output_format_str));
		if(count == -1){
			perror("Spice_Rack_App: store_measurement - Writing Measurements to File failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Writing Measurements to File failed - %s\n", strerror(errno));
			result = -1;
		}
	}
	else{
		//Will be inserting at current file position offset
		printf("Found existing Entry with same Spice Number. Replacing that Line\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Found existing Entry with same Spice Number. Replacing that Line\n");

		//Grab offset in file where matching line was found
		match_offset = lseek(temp_fd, 0, SEEK_CUR);
		if(match_offset  == -1){
			perror("Spice_Rack_App: store_measurement - Obtaining current offset in file failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Obtaining current offset in file failed - %s\n", strerror(errno));
			result = -1;
		}

		//Reset position to beginning of file
		file_offset = lseek(temp_fd, 0, SEEK_SET);
		if(file_offset  == -1){
			perror("Spice_Rack_App: store_measurement - Seeking to beginning of file failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Seeking to beginning of file failed - %s\n", strerror(errno));
			result = -1;
		}

		//copy existing file contents up to the match location
		if(match_offset > 0){
			if(copy_file(temp_fd, output_fd, match_offset) == -1){
				perror("Spice_Rack_App: store_measurement - Failed to copy temp file contents to output file - ");
				syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Failed to copy temp file contents to output file - %s\n", strerror(errno));
				result = -1;
			}
		}

		//Write new Entry
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Output String = %s", output_format_str);
		count = write(output_fd, output_format_str, strlen(output_format_str));
		if(count == -1){
			perror("Spice_Rack_App: store_measurement - Writing Measurements to File failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Writing Measurements to File failed - %s\n", strerror(errno));
			result = -1;
		}

		//Seek to EOF
		file_offset = lseek(temp_fd, eol, SEEK_SET);
		if(file_offset  == -1){
			perror("Spice_Rack_App: store_measurement - Seeking to EOF failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Seeking to EOF failed - %s\n", strerror(errno));
			result = -1;
		}		

		//copy existing file contents from end of replaced line to EOF
		if(copy_file(temp_fd, output_fd, EOF) == -1){
			perror("Spice_Rack_App: store_measurement - Failed to copy temp file contents to output file - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Failed to copy temp file contents to output file - %s\n", strerror(errno));

			result = -1;
		}
	}	

	close(output_fd);
	close(temp_fd);
	free(output_format_str);
	free(spice_num_str);
	return result;
}

static int consolidated_spice_file(){
	int full_fd, consolidated_fd;
	int i;

	consolidated_fd = open(CONSOLIDATED_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if(consolidated_fd == -1){
		perror("Spice_Rack_App: consolidated_spice_file - Failed to Open Consolidated File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: consolidated_spice_file - Failed to Open Consolidated File - %s", strerror(errno));
		return -1;
	}
	full_fd = open(OUTPUT_FILE, O_RDONLY, 0666);
	if(full_fd == -1){
		perror("Spice_Rack_App: consolidated_spice_file - Failed to Open Full File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: consolidated_spice_file - Failed to Open Full File - %s", strerror(errno));
		return -1;
	}
	
	for(i=2;i<(SPICE_RACK_SIZE+2);i++){
		write(consolidated_fd, spice_rack->spices[i].spice_entries.entries[1], strlen(spice_rack->spices[i].spice_entries.entries[1]));
		write(consolidated_fd, " - ", 3);
		write(consolidated_fd, spice_rack->spices[i].spice_entries.entries[TSP_COLUMN], strlen(spice_rack->spices[i].spice_entries.entries[TSP_COLUMN]));
		write(consolidated_fd, "tsp\n", 4);
	}

	close(full_fd);
	close(consolidated_fd);
	return 0;
}

static int read_weight(char *read_val, int read_len){
	int hx711_fd;
	int count = 0;
	int result = 0;
	
	hx711_fd = open(HX711_FILE, O_RDONLY);
	if(hx711_fd == -1){
		perror("Spice_Rack_App: read_weight - Failed to Open HX711 File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: read_weight - Failed to Open HX711 File - %s\n", strerror(errno));
		return -1;
	}
	while(read_len != 0 && (count = read(hx711_fd, read_val, read_len)) != 0){
		if(count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: read_weight - Reading weight failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: read_weight - Reading weight failed - %s\n", strerror(errno));
			result = -1;
			break;
		}
		read_len = read_len - count;
	}
	
	syslog(LOG_DEBUG,"Spice_Rack_App: read_weight - ADC Reading is %s", read_val);
	close(hx711_fd);
	return result;
}

static int get_average_weight(char *read_val, int read_len, int sample_num){
	int i;
	int sample_val = 0;
	long long sample_total = 0;
	int sample_average = 0;
	char *end_ptr;

	//Store previous adc reading in struct before collecting new ones
	spice_rack->previous_adc_reading = spice_rack->curr_adc_reading;

	//Collect readings and sum together
	for(i=0; i<sample_num; i++){
		if(read_weight(read_val, read_len) == -1){
			syslog(LOG_DEBUG, "Spice_Rack_App: get_average_weight - Failed to get ADC reading\n");
			i--;
			continue;
		}
		sample_val = strtol(read_val, &end_ptr, 10);
		sample_total = sample_total + sample_val;
	}

	//Calculate the average from the sum of the previous readings
	sample_average = sample_total/sample_num;
	syslog(LOG_DEBUG, "Spice_Rack_App: get_average_weight - Sample Average is %i\n", sample_average);

	//Convert average to string form which is used for the storing to file
	snprintf(read_val, strlen(read_val), "%i", sample_average);

	//Store new adc reading in struct 
	spice_rack->curr_adc_reading = sample_average;
	return sample_average;
}

static float adc_reading_to_grams(){
	float m = 0;
	int x = 0;
	float result;

	//Calculate the multiplier
	if(spice_rack->empty_jar_adc && spice_rack->empty_rack_adc){
		m = ((float)spice_rack->empty_jar_adc - spice_rack->empty_rack_adc)/spice_rack->empty_jar_mass;
	}

	//Calculate the offset
	if(spice_rack->curr_adc_reading > spice_rack->previous_adc_reading){
		x = spice_rack->curr_adc_reading - spice_rack->previous_adc_reading;
	}
	else{
		x = spice_rack->previous_adc_reading - spice_rack->curr_adc_reading;
	}

	syslog(LOG_DEBUG,"Spice_Rack_App: adc_reading_to_grams - x=%i and m=%f\n", x, m);
	result = x/m - spice_rack->empty_jar_mass;
	printf("Spice_Rack_App: adc_reading_to_grams - Result is %f grams\n", result);

	return result;
}

static int read_fsr_status(){
	int result = 0;
	int fsr_fd;
	int count = 0;
	int i = 0;
	int debounce_count = 0;
	unsigned char read_val;
	
	fsr_fd = open(FSR_FILE, O_RDONLY);
	if(fsr_fd == -1){
		perror("Spice_Rack_App: read_fsr_status - Failed to Open FSR Device File - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: read_fsr_status - Failed to Open FSR Device File - %s\n", strerror(errno));
		return -1;
	}

	//Loop to debounce the FSR sensor readings to ensure if we detect a change, it remains changed. 
	//Needed to extend to debounce for ~2 seconds because it is not only the FSR reading we want to 
	//settle but also the Weight sensor must settle too. Took measurements to see about how long it 
	//took for it to settle and added some guardband.
	while(i<10 && debounce_count<10){
		while((count = read(fsr_fd, &read_val, 1)) != 1){
			if(count == -1){
				if(errno == EINTR){
					continue;
				}
				perror("Spice_Rack_App: read_fsr_status - Reading FSR status failed - ");
				syslog(LOG_DEBUG, "Spice_Rack_App: read_fsr_status - Reading FSR status failed - %s\n", strerror(errno));
				result = -1;
				break;
			}
		}
		if(i == 0){
			result = read_val;
		}
		usleep(200000);
		i++;
		if(result == read_val){
			debounce_count++;
		}
		if(result != read_val){
			i = 0;
			debounce_count = 0;
		}
	}
	close(fsr_fd);
	return result;
}

static int read_in_calibration_data(){
	int fd;
	int i;
	char output_str[255];
	char *end_ptr;
	memset(output_str,0,255);


	fd = open(OUTPUT_FILE, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: read_in_calibration_data - Failed to open calibration data file - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: read_in_calibration_data - Failed to open calibration data file %s\n", OUTPUT_FILE);
		return -1;
	}

	for(i=0;i<(SPICE_RACK_SIZE+2);i++){
		if(read_line(fd, output_str) != 0){
			printf("read_line reported an issue.\n");
			syslog(LOG_DEBUG, "Spice_Rack_App: read_in_calibration_data - read_line reported issues\n");
			return -1;
		}
		parse_line(output_str,i);
	}

	spice_rack->empty_jar_adc = strtol(spice_rack->spices[1].spice_entries.entries[2], &end_ptr, 10);
	spice_rack->empty_rack_adc = strtol(spice_rack->spices[0].spice_entries.entries[2], &end_ptr, 10);

	close(fd);
	return 0;
}

static int read_calibrate_button(){
	int fd;
	int result;
	char read_val;
	char gpio_val_filename[29];

	snprintf(gpio_val_filename, 29, "/sys/class/gpio/gpio%s/value", CALIBRATE_GPIO);
	fd = open(gpio_val_filename, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: read_calibrate_button - Failed to /sys/class/gpio/gpioX/value - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: read_calibrate_button - Failed to /sys/class/gpio/gpioX/value - %s\n", strerror(errno));
		return -1;
	}
	result = read(fd, &read_val, 1);
	if(result != 1){
		perror("Spice_Rack_App: read_calibrate_button - Failed to read to /sys/class/gpio/gpioX/value - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: read_calibrate_button - Failed to read to /sys/class/gpio/gpioX/value - %s\n", strerror(errno));
		return -1;
	}
	close(fd);
	result = atoi(&read_val);
	return result;
}

static void *calibrate_button_routine(){
	int ret = 0;
	while(caught_signal == false){
		ret = read_calibrate_button();
		usleep(150000);
		if(ret == 1){
			printf("Calibrate Button Pressed\n");
			//Need mutex lock
			if(pthread_mutex_lock(&calibration.calibration_lock) != 0){
				perror("Unable to lock mutex\n");
			}
			else{
				calibration.calibration_button = 1;	
				pthread_mutex_unlock(&calibration.calibration_lock);
			}
		}
	}
	return 0;	
}

static int calibrate_spice_rack(char *read_val, int read_len){
	int fsr_status;
	int prev_fsr_status = 0;
	int fsr_diff = 0;
	int spice_num = 0;
	float mass = 0;
	float tsps = 0;
	char *user_input_val;
	char *end_ptr;
	char *spice_name;
	
	//Malloc Memory for Spice_Name field. Using MAX_FILE_ENTRY_LEN as length to ensure it isn't too big
	if((spice_name = (char *)malloc(MAX_FILE_ENTRY_LEN * sizeof(char))) == NULL){
		printf("Spice_Rack_App: calibrate_spice_rack - Failed on Malloc. Exiting Program\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Failed on Malloc. Exiting Program\n");
		return -1;
	}
	memset(spice_name, 0, MAX_FILE_ENTRY_LEN);

	//----------------------------EMPTY RACK----------------------------
	//Make sure rack is empty
	printf("Please remove all spices from Spice Rack to begin calibration\n");
	syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Please remove all spices from Spice Rack to begin calibration\n");
	while((fsr_status = read_fsr_status()) != 0){
		sleep(1);
	}
	printf("All spices have been removed. Collecting weight measurement of empty rack\n");
	syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - All spices have been removed. Collecting weight measurement of empty rack\n");

	//Collect ADC measurement
	spice_rack->empty_rack_adc = get_average_weight(read_val, read_len,10);
	syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Empty Rack Weight Reading is %s", read_val);
	
	//Store Measurement to file
	strcpy(spice_name, "Empty Rack");
	if(store_measurement(spice_num, spice_name, read_val, mass, tsps) != 0){
		printf("Error storing measurements to file\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Error storing measurements to file\n");
		return -1;
	}

	//----------------------------EMPTY JAR----------------------------
	//Checking if user wants to use Default Mass assumption for an Empty Jar
	printf("An Empty Spice Jar is assumed to be %f Grams.\n", spice_rack->empty_jar_mass);
	syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - An Empty Spice Jar is assumed to be %f Grams.\n", spice_rack->empty_jar_mass);
	printf("Do you wish to change this? (y/n)?)");
	syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Do you wish to change this? (y/n)?)\n");
	//Get User Input
	if((user_input_val = (char *)malloc(10 * sizeof(char))) == NULL){
		printf("Spice_Rack_App: calibrate_spice_rack - Failed on Malloc. Exiting Program\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Failed on Malloc. Exiting Program\n");
		return -1;
	}
	while(1){
		//Blank out on each loop to erase previous content
		memset(user_input_val,0,10);
		//Do you want to change the Default value for Mass of Empty Jar
		if(fgets(user_input_val, 10, stdin) != NULL){
			syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - user input is - %s\n", user_input_val);
			user_input_val[strcspn(user_input_val, "\n")] = 0;
		}
		else{
			perror("Spice_Rack_App: calibrate_spice_rack - fgets failed - ");
			syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - fgets failed - %s", strerror(errno));
			continue;
		}
		if(strcmp(user_input_val,"y") == 0){
			while(1){
				printf("Enter the new mass in grams that you wish to use. (Example: 128): ");
				syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Enter the new mass in grams that you wish to use\n");
				memset(user_input_val,0,10);
				//Get New Value of Mass of Empty Jar to Use
				if(fgets(user_input_val, 10, stdin) != NULL){
					user_input_val[strcspn(user_input_val, "\n")] = 0;
				}
				else{
					perror("Spice_Rack_App: calibrate_spice_rack - fgets failed - ");
					syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - fgets failed - %s", strerror(errno));
					continue;
				}
				spice_rack->empty_jar_mass = strtof(user_input_val, &end_ptr);
				if(spice_rack->empty_jar_mass == LONG_MIN || spice_rack->empty_jar_mass == LONG_MAX){
					perror("Spice_Rack_App: calibrate_spice_rack - User entered mass that is invalid - ");
					syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - User entered mass that is invalid - %s\n", strerror(errno));
					continue;
				}
				break;
			}
			break;
		}
		else if(strcmp(user_input_val, "n") == 0){
			break;
		}
		else{
			printf("Invalid Entry. Please enter y or n\n");
			syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Invalid Entry. Please enter y or n\n");
		}
	}
	free(user_input_val);	
	
	//Collect Mass Now
	printf("Place an empty jar on the spice rack now in spice1 position\n");
	syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Place an empty jar on the spice rack now in spice1 position\n");
	while(1){
		//Ensure Empty Jar is Placed on Spice Rack in spice1 position
		fsr_status = read_fsr_status();
		if(fsr_status != 1){
			continue;
		}
		//Collect ADC Measurement
		printf("Detected a jar was placed in Spice1 position. Beginning weighing now\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Detected a jar was placed in Spice1 position. Beginning weighing now\n");
		spice_rack->empty_jar_adc = get_average_weight(read_val, read_len, 10);
		printf("Done collecting measurement.\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Done collecting measurement.\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Empty Jar Weight Reading is %s", read_val);
		//Store Measurement
		memset(spice_name, 0, MAX_FILE_ENTRY_LEN);
		snprintf(spice_name, MAX_FILE_ENTRY_LEN, "Empty Jar-%ig", (int)spice_rack->empty_jar_mass);
		if(store_measurement(spice_num, spice_name, read_val, mass, tsps) != 0){
			printf("Error storing measurements to file\n");
			syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Error storing measurements to file\n");
			return -1;
		}
		break;
	}
	
	//Remove Empty Jar
	printf("Please remove empty jar from Spice1 location now\n");
	while(fsr_status != 0){
		fsr_status = read_fsr_status();
	}

	//Update spice_rack struct for calculations as spices are added.
	spice_rack->curr_adc_reading = spice_rack->empty_rack_adc;

	//----------------------------SPICES----------------------------

	printf("Now you will need to place and leave each spice on the rack. Only place one spice at a time when prompted to do so.\nGo ahead and place the first spice in Spice1 position\n");
	syslog(LOG_DEBUG, "Now you will need to place and leave each spice on the rack. Only place one spice at a time when prompted to do so.\nGo ahead and place the first spice in Spice1 position\n");
	while(1){
		spice_num = 1;
		//Wait for next spice to be added
		fsr_status = read_fsr_status();
		if(fsr_status == -1){
			syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Error Reading FSR\n");
			continue;
		}
		if(fsr_status != prev_fsr_status){
			fsr_diff = fsr_status - prev_fsr_status;
			if(fsr_diff < 0){
				continue;
			}
			while(fsr_diff != 1){
				spice_num = spice_num + 1;
				fsr_diff = fsr_diff >> 1;
			}
			printf("Detected a spice was placed in Spice%i position. Beginning weighing now\n", spice_num);
			syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Detected a spice was placed in Spice%i position. Beginning weighing now\n", spice_num);
			get_average_weight(read_val, read_len,10);
			printf("Done collecting measurement.\n");
			syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Done collecting measurement.\n");
			syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Weight Reading for Spice%i is %s", spice_num, read_val);
			//Convert ADC reading to grams
			mass = adc_reading_to_grams();

			while(1){
				//Name this Spice
				printf("Enter the name of this spice: ");
				syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Enter the name of this spice: ");
				memset(spice_name, 0, MAX_FILE_ENTRY_LEN);
				if(fgets(spice_name, MAX_FILE_ENTRY_LEN, stdin) != NULL){
					spice_name[strcspn(spice_name, "\n")] = 0;
					printf("Entered Spice Name is %s\n", spice_name);
					syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Entered Spice Name is %s\n", spice_name);
				}
				else{
					perror("Spice_Rack_App: calibrate_spice_rack - fgets failed - ");
					syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - fgets failed - %s", strerror(errno));
				}
				//Convert grams to tsp and check if valid spice name 
				tsps = convert_grams_to_tsp(spice_name, mass);
				if(tsps != -1){
					break;
				}
				print_spice_list();
			}
			//Store measurement
			if(store_measurement(spice_num, spice_name, read_val, mass, tsps) != 0){
				printf("Error storing measurements to file\n");
				syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Error storing measurements to file\n");
				return -1;
			}
			prev_fsr_status = fsr_status;
			if(spice_num == SPICE_RACK_SIZE){
				break;
			}
			else{
				printf("Place the next desired spice on the Spice Rack now\n");
			}

		}
	}
	free(spice_name);

	//Produce a consolidated data file for TCP socket queries
	if(consolidated_spice_file() != 0){
		printf("Spice_Rack_App: calibrate_spice_rack - Failed to create consolidated spice file\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Failed to create consolidated spice file\n");
	}

	printf("Finished Calibration.\n");
	syslog(LOG_DEBUG, "Finished Calibration.\n");

	return 0;
}

static int setup_calibrate_button(){
	int fd;
	int result;
	char direction_filename[40];

	//Export GPIO (calibrate button)
	fd = open("/sys/class/gpio/export", O_WRONLY);
	if(fd == -1){
		perror("Spice_Rack_App: setup_caibrate_button - Failed to /sys/class/gpio/export - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: setup_caibrate_button - Failed to /sys/class/gpio/export - %s\n", strerror(errno));
		return -1;
	}
	result = write(fd, CALIBRATE_GPIO, 2);
	if(result != 2){
		perror("Spice_Rack_App: setup_caibrate_button - Failed to write to /sys/class/gpio/export - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: setup_caibrate_button - Failed to write to /sys/class/gpio/export - %s\n", strerror(errno));
		return -1;
	}
	close(fd);
	
	//Set direction of GPIO to "in"
	snprintf(direction_filename, 34, "/sys/class/gpio/gpio%s/direction", CALIBRATE_GPIO);
	fd = open(direction_filename, O_WRONLY);
	if(fd == -1){
		perror("Spice_Rack_App: setup_caibrate_button - Failed to open GPIO direction file - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: setup_caibrate_button - Failed to open %s - %s\n", direction_filename, strerror(errno));
		return -1;
	}
	result = write(fd, "in", 2);
	if(result != 2){
		perror("Spice_Rack_App: setup_calibrate_button - Failed to write to GPIO direction file - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: setup_caibrate_button - Failed to write to %s - %s\n", direction_filename, strerror(errno));
		return -1;
	}

	//Initialize Pthread mutex and launch thread to monitor calibrate button
	if(pthread_mutex_init(&calibration.calibration_lock,NULL) != 0){
		perror("Spice_Rack_App: setup_calibration_button - Failed to initialize Mutex - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: setup_calibration_button - Failed to initialize Mutex - %s\n", strerror(errno));
		return -1;
	}
	if(pthread_create(&calibration.calibrate_thread, NULL, calibrate_button_routine,NULL) != 0){
		perror("Spice_Rack_App: setup_calibration_button - Unable to create calibrate button thread - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: setup_calibration_button - Unable to create calibrate button thread - %s\n", strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}

int free_calibrate_button(){
	int fd;
	int result;

	//Unexport GPIO (calibrate button)
	fd = open("/sys/class/gpio/unexport", O_WRONLY);
	if(fd == -1){
		perror("Spice_Rack_App: free_calibrate_button - Failed to /sys/class/gpio/unexport - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: free_calibrate_button - Failed to /sys/class/gpio/unexport - %s\n", strerror(errno));
		return -1;
	}
	result = write(fd, CALIBRATE_GPIO, 2);
	if(result != 2){
		perror("Spice_Rack_App: free_calibrate_button - Failed to write to /sys/class/gpio/unexport - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: free_calibrate_button - Failed to write to /sys/class/gpio/unexport - %s\n", strerror(errno));
		return -1;
	}
	close(fd);
	return 0;
}

static int setup_spice_rack_struct(){
	int i;
	int result = 0;
	int size = MAX_FILE_ENTRY_LEN;
	int num_entries = SPICE_RACK_SIZE + 2;


	if((spice_rack = (struct spice_rack *)malloc(sizeof(struct spice_rack) + (num_entries*sizeof(struct spice)) + (2*(num_entries * sizeof(char[size]))))) == NULL){
		result = -1;
	}

	for(i=0;i<num_entries;i++){
		//Malloc Strings for file entries
		if((spice_rack->spices[i].spice_search_strings.search_strings[0] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_search_strings.search_strings[1] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_search_strings.search_strings[2] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_search_strings.search_strings[3] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_search_strings.search_strings[4] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_entries.entries[0] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_entries.entries[1] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_entries.entries[2] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_entries.entries[3] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if((spice_rack->spices[i].spice_entries.entries[4] = (char *)malloc(size*sizeof(char))) == NULL){
			result = -1;
		}
		if(result == -1){
			printf("Spice_Rack_App: setup_spice_rack_struct - Failed on Malloc. Exiting Program\n");
			syslog(LOG_DEBUG, "Spice_Rack_App: setup_spice_rack_struct - Failed on Malloc. Exiting Program\n");
			return result;
		}
		memset(spice_rack->spices[i].spice_search_strings.search_strings[0],0,size);
		memset(spice_rack->spices[i].spice_search_strings.search_strings[1],0,size);
		memset(spice_rack->spices[i].spice_search_strings.search_strings[2],0,size);
		memset(spice_rack->spices[i].spice_search_strings.search_strings[3],0,size);
		memset(spice_rack->spices[i].spice_search_strings.search_strings[4],0,size);
		strncpy(spice_rack->spices[i].spice_search_strings.search_strings[0], "Spice_Location:", 16);
		strncpy(spice_rack->spices[i].spice_search_strings.search_strings[1], "Spice_Name:", 12);
		strncpy(spice_rack->spices[i].spice_search_strings.search_strings[2], "ADC_Reading:", 13);
		strncpy(spice_rack->spices[i].spice_search_strings.search_strings[3], "Calibrated_Mass(grams):", 24);
		strncpy(spice_rack->spices[i].spice_search_strings.search_strings[4], "Teaspoons:", 11);
		memset(spice_rack->spices[i].spice_entries.entries[0],0,size);
		memset(spice_rack->spices[i].spice_entries.entries[1],0,size);
		memset(spice_rack->spices[i].spice_entries.entries[2],0,size);
		memset(spice_rack->spices[i].spice_entries.entries[3],0,size);
		memset(spice_rack->spices[i].spice_entries.entries[4],0,size);
      	}

	return result;
}

static int cleanup_spice_rack_struct(){
	int i;
	int num_entries = SPICE_RACK_SIZE + 2;
	for(i=0;i<num_entries;i++){
		free(spice_rack->spices[i].spice_search_strings.search_strings[0]); 
		free(spice_rack->spices[i].spice_search_strings.search_strings[1]); 
		free(spice_rack->spices[i].spice_search_strings.search_strings[2]); 
		free(spice_rack->spices[i].spice_search_strings.search_strings[3]); 
		free(spice_rack->spices[i].spice_search_strings.search_strings[4]); 
		free(spice_rack->spices[i].spice_entries.entries[0]);
		free(spice_rack->spices[i].spice_entries.entries[1]);
		free(spice_rack->spices[i].spice_entries.entries[2]);
		free(spice_rack->spices[i].spice_entries.entries[3]);
		free(spice_rack->spices[i].spice_entries.entries[4]);
      	}
	return 0;
}

static void fsr_monitor(union sigval sigval){
	struct thread_data *td = (struct thread_data*) sigval.sival_ptr;
	if(pthread_mutex_lock(&td->lock) == 0){	
		td->fsr_prev_status = td->fsr_cur_status;
		td->fsr_cur_status = read_fsr_status();
		if(td->fsr_cur_status != td->fsr_prev_status){
			td->fsr_alert = 1;
		}
		pthread_mutex_unlock(&td->lock);
	}
	return;
}

static int setup_fsr_status_timer(struct sigevent *sigev, timer_t *timerid, struct thread_data *td, struct itimerspec *timerspec, struct timespec *start_time){
	td->hb = 0;
	td->fsr_alert = 0;
	td->fsr_cur_status = read_fsr_status();
	if(pthread_mutex_init(&td->lock,NULL) != 0){
		perror("Spice_Rack_App: setup_fsr_status_timer - Failed to initiate timer mutex - ");
		syslog(LOG_DEBUG,"Spice_Rack_App: setup_fsr_status_timer - Failed to initiate timer mutex - %s", strerror(errno));
		return -1;
	}
	sigev->sigev_notify = SIGEV_THREAD;
	sigev->sigev_value.sival_ptr = td;
	sigev->sigev_notify_function = fsr_monitor;
	timerspec->it_interval.tv_sec = 5;
	timerspec->it_interval.tv_nsec = 0;
	if(clock_gettime(CLOCK_MONOTONIC, start_time) != 0){
		perror("Spice_Rack_App: setup_fsr_status_timer - Error getting current time - ");
		syslog(LOG_DEBUG,"Spice_Rack_App: setup_fsr_status_timer - Error getting current time - %s", strerror(errno));
		return -1;
	}
	
	start_time->tv_sec = start_time->tv_sec + 2;
	if(timer_create(CLOCK_MONOTONIC, sigev, timerid) != 0){
		perror("Spice_Rack_App: setup_fsr_status_timer - Unable to create timer - ");
		syslog(LOG_DEBUG,"Spice_Rack_App: setup_fsr_status_timer - Unable to create timer - %s", strerror(errno));
		return -1;
	}
	else{
		syslog(LOG_DEBUG,"Spice_Rack_App: setup_fsr_status_timer - Successfully setup timer\n");
		timerspec->it_value.tv_sec = start_time->tv_sec;
		if(timer_settime(*timerid, TIMER_ABSTIME, timerspec, NULL) != 0){
			perror("Spice_Rack_App: setup_fsr_status_timer - Unable to start timer - ");
			syslog(LOG_DEBUG,"Spice_Rack_App: setup_fsr_status_timer - Unable to start timer - %s", strerror(errno));
			return -1;
		}
		syslog(LOG_DEBUG, "Spice_Rack_App: setup_fsr_status_timer - Successfully started timer\n");
	}	

	return 0;
}

static int convert_fsr_stat_to_spice_num(int spice_num){
	int i;
	for(i=0; i < SPICE_RACK_SIZE; i++){
		if((spice_num >> i) == 1){
			spice_num = i+1;
			break;
		}
	}
	return spice_num;
}

static int update_spice_rack(int spice_num, char *spice_name, char *read_val, float mass, float tsps){
	char *mass_str;
	char *tsps_str;

	if((mass_str = (char *)malloc(MAX_FILE_ENTRY_LEN * sizeof(char))) == NULL){
		perror("Spice_Rack_App: update_spice_rack - Failed on Malloc.");
		syslog(LOG_DEBUG, "Spice_Rack_App: update_spice_rack - Failed on Malloc - %s\n", strerror(errno));
	}
	memset(mass_str, 0, MAX_FILE_ENTRY_LEN);
	if((tsps_str = (char *)malloc(MAX_FILE_ENTRY_LEN * sizeof(char))) == NULL){
		perror("Spice_Rack_App: update_spice_rack - Failed on Malloc.");
		syslog(LOG_DEBUG, "Spice_Rack_App: update_spice_rack - Failed on Malloc. - %s\n", strerror(errno));
	}
	memset(tsps_str, 0, MAX_FILE_ENTRY_LEN);

	strcpy(spice_rack->spices[spice_num+1].spice_entries.entries[ADC_COLUMN], read_val);
	snprintf(mass_str, MAX_FILE_ENTRY_LEN, "%3.6f", mass);
	strcpy(spice_rack->spices[spice_num+1].spice_entries.entries[MASS_COLUMN], mass_str);
	snprintf(tsps_str, MAX_FILE_ENTRY_LEN, "%3.6f", tsps);
	strcpy(spice_rack->spices[spice_num+1].spice_entries.entries[TSP_COLUMN],tsps_str);

	free(mass_str);
	free(tsps_str);
	return 0;
}

int main() {
	struct sigaction socket_sigaction;

	char *read_val;
	int read_len = 8;
	int fd;
	int spice_num;
	float mass = 0;
	float tsps = 0;
	char spice_name[32];
	struct sigevent sigev;
	struct itimerspec timerspec;
	struct timespec start_time;
	struct thread_data td;
	timer_t timerid;

	//Logging
	openlog(NULL,0,LOG_USER);
	syslog(LOG_DEBUG,"Spice_Rack_App: Starting Application");

	//Setup Signal Handler
        memset(&socket_sigaction,0,sizeof(struct sigaction));
        socket_sigaction.sa_handler=socket_signal_handler;
        if(sigaction(SIGTERM, &socket_sigaction, NULL) != 0) {
                perror("Spice_Rack_App: main - Error registering for SIGTERM -  ");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Error registering for SIGTERM - %s\n", strerror(errno));

        }
        if(sigaction(SIGINT, &socket_sigaction, NULL) != 0) {
                perror("Spice_Rack_App: main - Error registering for SIGINT - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Error registering for SIGINT - %s\n", strerror(errno));

        }

	//Setup Calibration Button and Launch Thread to Monitor Status
	if(setup_calibrate_button() != 0){
		printf("Spice_Rack_App: main - error in setting up the calibration button. Exiting program\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - error in the setup calibration button function. Exiting program\n");
	}

	//Malloc String for Storing ADC measurements in
	if((read_val = (char *)malloc(read_len * sizeof(char))) == NULL){
		perror("Spice_Rack_App: main - Failed on Malloc. Exiting Program - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed on Malloc. Exiting Program - %s\n", strerror(errno));
	}
	memset(read_val,0,read_len);

	
	//Initialize Spice Rack Struct
	if(setup_spice_rack_struct() != 0){
	//	return -1;
	}	
	spice_rack->curr_adc_reading = 0;
	spice_rack->empty_jar_mass = EMPTY_JAR_MASS_DEF;
	printf("Collecting Weight Measurement now\n");
	syslog(LOG_DEBUG, "Spice_Rack_App: main - Collecting Weight Measurement now\n");
	get_average_weight(read_val, read_len, 10);
	printf("Done collecting weight\n");
	syslog(LOG_DEBUG, "Spice_Rack_App: main - Done collecting weight\n");
	
	//Check for Previous Calibration Data
	fd = open(OUTPUT_FILE, O_RDONLY);
	if(fd == -1){
		printf("Unable to find previous calibration data to use. Performing a new calibration\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Unable to find previous calibration data to use. Performing a new calibration\n");
		//Calibrate if none found
		if(calibrate_spice_rack(read_val, read_len) != 0){
			printf("Spice_Rack_App: main - Failed in calibrating spice rack\n");
			syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed in calibrating spice rack\n");
		}
	}
	else{
		printf("Found previous calibration data to use. To perform new calibration press the calibration button\n");	
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Found previous calibration data to use. Using found calibration data\n");
	}

	//Read in Calibration Data to Spice Rack Struct
	if(read_in_calibration_data() != 0){
		printf("Spice_Rack_App: main - Failed to read in calibration data\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed to read in calibration data\n");
	}

	//Produce a consolidated data file for TCP socket queries
	if(consolidated_spice_file() != 0){
		printf("Spice_Rack_App: main - Failed to create consolidated spice file\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed to create consolidated spice file\n");
	}

	
	//Setup timer for monitoring for changes in fsr (Taking off or Putting Back Spices)
	memset(&sigev, 0, sizeof(struct sigevent));
	memset(&timerspec, 0, sizeof(struct itimerspec));
	memset(&td, 0, sizeof(struct thread_data));
	if(setup_fsr_status_timer(&sigev, &timerid, &td, &timerspec, &start_time) != 0){
		printf("Spice_Rack_App: main - Failed to setup FSR timer\n");
		syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed to setup FSR timer\n");
		return -1;
	}

	printf("Application is now initialized and running...\n");
	while(1){
		spice_num = 0;
		if(pthread_mutex_lock(&td.lock) == 0){
			if(td.fsr_alert == 1){
				td.fsr_alert = 0;
				if(td.fsr_cur_status > td.fsr_prev_status){
					//If a spice was added back
					spice_num = td.fsr_cur_status - td.fsr_prev_status;
					spice_num = convert_fsr_stat_to_spice_num(spice_num);
					printf("Added spice%i\n", spice_num);
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Added spice%i\n", spice_num);
					printf("Collecting Weight Measurement now\n");
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Collecting Weight Measurement now\n");
					get_average_weight(read_val, read_len, 10);
					printf("Done collecting weight\n");
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Done collecting weight\n");
					mass = adc_reading_to_grams();
					strncpy(spice_name, spice_rack->spices[spice_num+1].spice_entries.entries[1],32);
					tsps = convert_grams_to_tsp(spice_name, mass);
					update_spice_rack(spice_num, spice_name, read_val, mass, tsps);
					if(store_measurement(spice_num, spice_name, read_val, mass, tsps) != 0){
						printf("Error storing measurements to file\n");
						syslog(LOG_DEBUG, "Spice_Rack_App: calibrate_spice_rack - Error storing measurements to file\n");
					}
					//Produce a consolidated data file for TCP socket queries
					if(consolidated_spice_file() != 0){
						printf("Spice_Rack_App: main - Failed to create consolidated spice file\n");
						syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed to create consolidated spice file\n");
					}
				}
				else{
					//If a spice was removed
					spice_num = td.fsr_prev_status - td.fsr_cur_status;
					spice_num = convert_fsr_stat_to_spice_num(spice_num);
					printf("Removed Spice%i\n", spice_num);
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Removed Spice%i\n", spice_num);
					//Collects weight and updates the prev and curr adc readings in struct
					printf("Collecting Weight Measurement now\n");
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Collecting Weight Measurement now\n");
					get_average_weight(read_val, read_len, 10);
					printf("Done collecting weight\n");
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Done collecting weight\n");
				}
				
			}
			pthread_mutex_unlock(&td.lock);
		}
		if(pthread_mutex_lock(&calibration.calibration_lock) == 0){
			if(calibration.calibration_button == 1){
				calibrate_spice_rack(read_val, read_len);
				//Read in Calibration Data to Spice Rack Struct
				if(read_in_calibration_data() != 0){
					printf("Spice_Rack_App: main - Failed to read in calibration data\n");
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed to read in calibration data\n");
				}

				//Produce a consolidated data file for TCP socket queries
				if(consolidated_spice_file() != 0){
					printf("Spice_Rack_App: main - Failed to create consolidated spice file\n");
					syslog(LOG_DEBUG, "Spice_Rack_App: main - Failed to create consolidated spice file\n");
				}
				calibration.calibration_button = 0;
			}
			pthread_mutex_unlock(&calibration.calibration_lock);
		}
		if(caught_signal == true){
                        syslog(LOG_DEBUG, "SOCKET: Caught signal, exiting");
			timer_delete(timerid);
			cleanup_spice_rack_struct();
        		free(spice_rack);
        		free(read_val);
	        	free_calibrate_button();
			pthread_join(calibration.calibrate_thread, NULL);
			closelog();
			return 0;
		}
			
	}
}
