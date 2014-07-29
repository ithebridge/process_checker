#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h> 
#include <ctype.h>

#define MAX_METRICS 14


const char *process_dir = "/proc";

//Simple stucture to hold number and array of stats
struct process_info {
	int stats_added;
	char* stats[ MAX_METRICS ];
};

//We can expand this as we add config options;
struct config {
	int graphite_port;	
	int sleep_time;
	char* graphite_ip;
};

//Function to test if a string is numeric
int is_numeric( const char * s ){
    if (s == NULL || *s == '\0' || isspace( *s ) ){
      return 0;
	}
    char * p;
    strtod( s, &p ); //strips numeric part of a string out
    return *p == '\0'; //If anything is remaining then s is not just a number
}

/* Read an entire file into a string
	RETURNS a malloc'ed string that must be freed
*/
char* read_all_file( const char* input_file ){
	int buffer_size = 256;
	char *file_contents = malloc( buffer_size );
	file_contents[ 0 ] = '\0';
	FILE *file_stream = NULL;
	char buffer[ buffer_size ];
	ssize_t length_read = 0;
	int open_failed = 0;

	file_stream = fopen( input_file, "r" );
	//Make sure that the file could be opened
	if( file_stream == NULL ){
		printf( "Could not open file %s\n", input_file );
		open_failed = 1;
	}
	//If it opened
	if( open_failed == 0 ){
		while( fgets( buffer, buffer_size, file_stream ) != NULL ){
			length_read += buffer_size;
			file_contents = realloc( file_contents, length_read + 1 );
			strcat( file_contents, buffer );
		}	
	}
	fclose( file_stream );
	return file_contents;
}


/*
	Call the passed function with the process_info struct as an arg on each line of a file
*/
void call_on_line( const char* input_file, void( *action )( char *, struct process_info * ), struct process_info *data ){
	char *line;
	FILE *file_stream = NULL;
	ssize_t read; size_t len = 0;
	int open_failed = 0;
	
	//Open passed filename for reading	
	file_stream = fopen( input_file, "r" );

	//We couldn't open the file, set open_failed so we don't try and read file.
	if( file_stream == NULL ){
		printf( "Could not open file %s\n", input_file );
		open_failed = 1;
	}

	if( open_failed == 0){

		while( ( read = getline( &line, &len, file_stream ) ) != -1 ){
			// Call passed function to parse line struct and add to data
			action( line, data );
		}
		if( line ){
			free( line );
		}

		fclose( file_stream );
	}

	return;
}


void process_io( char *line, struct process_info *data ){
	
	int sno = data->stats_added;
	//We only need to malloc strlen and not strlen + 1 as we split on
	char *l = malloc( strlen( line ) );
	char *info;
	char *save;

	//We split the line on ":"
	info = strtok_r( line, ":", &save );
	//Copy the first token we find into l, this is the name of the var in graphite 
	memcpy( l, info, strlen( info ) + 1);
	//Get the next token
	info = strtok_r( NULL, ":", &save);
	//We have to strip the final new line char if it exists
	if( info[ strlen( info ) - 1 ]  == '\n' ){
		info[ strlen( info ) - 1 ] = '\0';
	}	
	strcat( l, info );
	//Add to data and increase the number of stats we have registered
    data->stats[ sno ] = l ;
    data->stats_added = sno + 1;
	return;
}

// Parse statm file and add lines into data file
void process_statm( char *line, struct process_info *data ){
	char *info, *save;
	//List of column names in statm file
	char *columns[] = { "size", "resident", "share", "text", "lib", "data", "dt" };
	int i = 0;
	int first_run = 1;

	while( ( info = strtok_r( first_run == 1 ? line : NULL, " ", &save ) ) != NULL ){
		// Allocate memory, +2 to accomodate for the newline
		char *l = calloc(1, strlen( info ) + strlen( columns[ i ] ) + 2 );
		// Add in the column name
		memcpy( l, columns[ i ], strlen( columns[ i ] ) + 1 );
		strcat( l, " " );
		// Strip the new line char if present
		if( info[ strlen( info ) - 1 ]  == '\n' ){
			info[ strlen( info ) - 1 ] = 0;
		}
		strcat( l, info );
		data->stats[ data->stats_added ] = l ;
		data->stats_added++;
		i++;
		first_run = 0;
	}
	return;
}

// Call the passed function on each line of the supplied file
void process_file( char *subprocdir, char* info_file, void( *action )( char *, struct process_info * ), struct process_info *data ){
	//Create the full filename of the file we are going to read
	char *file_to_read = malloc( strlen( subprocdir ) + strlen( info_file ) + 1 );
	memcpy( file_to_read, subprocdir, strlen( subprocdir ) + 1 );
	strcat( file_to_read, info_file );
	//call function with full filepath
	call_on_line( file_to_read, action, data );
	free( file_to_read );
	return;
}

//Parse the cmdline file, gets the last part of the filepath and removes some invalid graphite chars
void parse_cmd_line( char *process ){
	char *cmd, *save, *tmp;
	int i;
	int first_run = 1;
	//Create a copy of process string to tokenise it.
	char *t = malloc( strlen( process ) + 1 );
	memcpy( t, process, strlen( process ) + 1 );
	//Loop through parts and leave cmd with the final part of filepath
	while( ( tmp = strtok_r( ( first_run == 1 ? t : NULL ), "/", &save ) ) != NULL ){
		cmd = tmp;
		if( first_run == 1){
			first_run = 0;
		}	
	}
	//If cmd is preset
	if( cmd != NULL ){
		//Strip out all chars that are invalid
		for( i = 0; i < strlen( cmd ); i++ ){
			if( cmd[ i ] == ' ' || cmd[ i ] == ':' ){
				cmd[ i ] = '_';
			}
		}
		//Copy the parsed cmdline into process
		memcpy( process, cmd, strlen( cmd ) + 1 );
	}
	free( t );
	return;
}

// Main list function
int list_processes( struct config *config_options ){
	struct dirent *proc;
    DIR *dir;
    struct stat buf;

	// Get the hostname for graphite main dir
	char hostname[ 64 ];	
	gethostname( hostname, 64 );

	// Get timestamp for graphite
	char *timestamp = calloc( 1, 11 );

	struct tm *tstruct;
	time_t t = time( NULL );
	tstruct = localtime( &t );
	strftime( timestamp, 11, "%s", tstruct );

	int sock;
	struct sockaddr_in server;
	printf( "Graphite_ip %s\n", config_options->graphite_ip );
	char *server_ip = config_options->graphite_ip;
	printf( "Graphite port %d\n", config_options->graphite_port );
	unsigned int server_port = config_options->graphite_port;

	if( ( sock = socket( PF_INET, SOCK_STREAM, IPPROTO_TCP ) ) < 0 ){
		printf( "socket() failed" );
		exit( 1 );
	}
	memset( &server, 0, sizeof( server ) );
	server.sin_family      = AF_INET;
	server.sin_addr.s_addr = inet_addr( server_ip );  
	server.sin_port        = htons( server_port ); 

	if( connect( sock, (struct sockaddr *) &server, sizeof( server ) ) < 0 ){
		printf("connect() failed");
		exit( 1 );
	}

	dir = opendir( process_dir );
	if( dir == NULL ){
//		printf( "Could not open %s for reading\n", process_dir );
		exit( 1 );
	}


	while( ( proc = readdir( dir ) ) != NULL ){

		if( is_numeric( proc->d_name ) ){
			
			char *subprocdir = malloc( strlen( process_dir ) + strlen( proc->d_name ) + 3 );
			memcpy( subprocdir, process_dir, strlen( process_dir ) + 1 );
			strcat( subprocdir, "/" );
			strcat( subprocdir, proc->d_name );
			strcat( subprocdir, "/" );
			
			char *proc_file = malloc( strlen( subprocdir ) + 8 );
			memcpy( proc_file, subprocdir, strlen( subprocdir ) + 1 );
			strcat( proc_file, "cmdline" );

			char *process_arg = read_all_file( proc_file );
			

			
			if( process_arg != NULL && process_arg[0] != '\0' ){	
				struct process_info *info = calloc(1, sizeof( struct process_info ) );
				info -> stats_added = 0;
				process_file( subprocdir, "statm", process_statm, info );
				process_file( subprocdir, "io", process_io, info );
				parse_cmd_line( process_arg );
//				printf( "proc: %s\n", process_arg );
				int i;
				for( i = 0; i < info -> stats_added; i++ ){
					char* stat = malloc( strlen( hostname ) + strlen( process_arg ) + strlen( info->stats[ i ] ) + strlen( timestamp ) + 5 );
					memcpy( stat, hostname, strlen( hostname ) + 1 );
					strcat( stat, "." );
					strcat( stat, process_arg );
					strcat( stat, ".");
					strcat( stat, info->stats[ i ] );
					strcat( stat, " " );
					strcat( stat, timestamp );
					strcat( stat, "\n" );
					int sent;
//					printf( "stat: %s\n", stat );
					if ( ( sent = ( send( sock, stat, strlen( stat ), 0) ) != strlen( stat ) ) ){
						printf("send() sent %i bytes\n", sent);
					}
					free( info->stats[ i ] );
					free( stat );
				}
				free( info );
			}

			free( process_arg );	
			free( subprocdir );
			free( proc_file );
		}
	}
	free( proc );
	free( timestamp );
	closedir( dir );
	close( sock );
	return 1; 
}

struct config *parse_config_file( char* file_location ){
	struct config *config_options = calloc( 1, sizeof( *config_options ) );
	printf( "config location %s\n", file_location );
    FILE *file_stream = NULL;
    int open_failed = 0;
	char *line;
	int read;
	size_t len = 0;

	//Set some default options here.... Or should we just exit if not all options are specified...
	config_options-> graphite_ip = calloc(1, 16 );
	strcat( config_options-> graphite_ip, "127.0.0.1" );
	config_options-> graphite_port = 2003;
	config_options-> sleep_time = 30;

	//Open passed filename for reading  
	file_stream = fopen( file_location, "r" );

	//We couldn't open the file, set open_failed so we don't try and read file.
	if( file_stream == NULL ){
		printf( "Could not open file %s\n", file_location );
		open_failed = 1;
	}

	if( open_failed == 0 ){
		while( ( read = getline( &line, &len, file_stream ) ) != -1 ){
			if( line[ 0 ] == '#' ){
				printf( "Comment line - %s\n", line );
			
				continue;
			} else if ( strstr( line, "graphite_ip" ) ){
				// This is an awful way to do this, but i'm tired
				memcpy( config_options->graphite_ip, line + 12, 15 );
				continue;	
			} else if( strstr( line, "graphite_port" ) ){
				//Again, needs improvement
				char* tmp;
				double port = strtod( &line[14], &tmp );
				config_options -> graphite_port = (int) port;	
				continue;
			} else if( strstr( line, "sleep_time" ) ){
				//Again, needs improvement
				char* tmp;
				double port = strtod( &line[10], &tmp );
				config_options -> sleep_time = (int) port;	
				continue;
			}	else {
				printf( "Unknown config option %s found.... Ignoring\n", line );
				continue;
			}
			
		}
		if( line ){
			free( line );
		}
	}
	fclose( file_stream );
	return config_options;	


}


int main( int argc, char *argv[] ){
	struct config *config_options = parse_config_file( "/etc/process_checker/process_checker.conf" );
	printf( "config_opt %s\n", config_options -> graphite_ip );
	while( 1 ){
		list_processes( config_options );
		sleep( config_options->sleep_time );
	}
	free( config_options-> graphite_ip );
	free( config_options );
}
