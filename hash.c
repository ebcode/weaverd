#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "weaver.h"
#include "config.h"
#include "../mdb/util.h"
#include "hash.h"

static char *string_storage = NULL;
static int string_storage_length = INITIAL_STRING_STORAGE_LENGTH;
static int next_string = 0;
static int string_storage_file = 0;

static int *string_storage_table = NULL;
static int string_storage_table_length = STRING_STORAGE_TABLE_LENGTH;

static int *group_table = NULL;
static int group_table_length = GROUP_TABLE_LENGTH;
static int next_group_id = 0;
static int group_file = 0;

static int inhibit_file_write = 0;

#define HASH_GRANULARITY 1024

/* one_at_a_time_hash() */

unsigned int hash(const char *key, unsigned int len, unsigned int table_length)
{
  unsigned int   hash, i;
  for (hash=0, i=0; i<len; ++i)
  {
    hash += key[i];
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return (hash & (table_length - 1));
} 

char *get_string(offset) {
  return string_storage + offset;
}

void extend_string_storage(void) {
  int new_length = string_storage_length * 2;
  char *new_string_storage = cmalloc(new_length);
  memcpy(new_string_storage, string_storage, string_storage_length);
  free(string_storage);
  string_storage = new_string_storage;
  string_storage_length = new_length;
}

unsigned int enter_string_storage(const char *string) {
  int string_length = strlen(string);
  int search = hash(string, string_length, string_storage_table_length);
  int offset;

  printf("Entering '%s'\n", string);

  while (1) {
    offset = string_storage_table[search];
    if (! offset)
      break;
    else if (offset && 
	     ! strcmp(string, (string_storage + offset)))
      break;
    if (search++ >= string_storage_table_length)
      search = 0;
  }

  if (! offset) {
    if (next_string + string_length > string_storage_length)
      extend_string_storage();
    strcpy((string_storage + next_string), string);
    if (! inhibit_file_write)
      write(string_storage_file, string, string_length + 1);
    string_storage_table[search] = next_string;
    offset = next_string;
    next_string += string_length + 1;
  }

  return offset;
}

void populate_string_table_from_file(int fd) {
  loff_t fsize = file_size(fd);
  loff_t bytes_read = 0;
  char buffer[MAX_STRING_SIZE], *c;
  int result;

  while (bytes_read < fsize) {
    c = buffer;
    while (1) {
      while ((result = read(fd, c, 1)) < 1) {
	if (result < 0)
	  perror("Reading string file");
      }
      bytes_read++;
      if (*c == 0) 
	break;
      c++;
    }
    enter_string_storage(buffer);
  }
}

void init_string_hash (void) {
  string_storage = cmalloc(string_storage_length);
  string_storage_table =
    (int*)cmalloc(STRING_STORAGE_TABLE_LENGTH * sizeof(int));

  if ((string_storage_file = open64(index_file_name(STRING_STORAGE_FILE),
			      O_RDWR|O_CREAT, 0644)) == -1)
    merror("Opening the string storage file.");

  if (file_size(string_storage_file) == 0) {
    write_from(string_storage_file, "@@@", 4);
    next_string = 4;
  } else 
    populate_string_table_from_file(string_storage_file);
}

node *get_node(const char *message_id, unsigned int group_id) {
}


/*** Groups ***/

group *get_group(const char *group_name) {
  int string_length = strlen(group_name);
  int search = hash(group_name, string_length, group_table_length);
  int offset, group_id;
  group *g;

  printf("Entering group '%s'\n", group_name);

  while (1) {
    offset = group_table[search];
    if (! offset)
      break;
    else if (offset && ! strcmp(group_name,
				get_string(groups[offset].group_name)))
      break;
    if (search++ >= group_table_length)
      search = 0;
  }

  if (! offset) {
    group_id = next_group_id++;
    if (! inhibit_file_write) {
      g = &groups[group_id];
      g->group_name = enter_string_storage(group_name);
      g->group_description = enter_string_storage("");
      g->group_id = group_id;
      g->dirtyp = 1;
    }
    offset = group_id;
    group_table[search] = group_id;
  } 

  return &groups[offset];
}

void flush_groups(void) {
  int i;
  group *g;

  for (i = 0; i<next_group_id; i++) {
    g = &groups[i];
    if (g->dirtyp) {
      g->dirtyp = 0;
      lseek64(group_file, (loff_t)i * sizeof(group), SEEK_SET);
      write_from(group_file, (char*)(&groups[i]), sizeof(group));
    }
  }
}

void populate_group_table_from_file(int fd) {
  loff_t fsize = file_size(fd);
  int i;

  for (i = 0; i<fsize/sizeof(group); i++) {
    read_block(fd, (char*)(&groups[i]), sizeof(group));
    if (groups[i].group_name)
      get_group(get_string(groups[i].group_name));
  }
}

void init_group_hash (void) {
  group_table = (int*)cmalloc(MAX_GROUPS * sizeof(int));

  if ((group_file = open64(index_file_name(GROUP_FILE),
			   O_RDWR|O_CREAT, 0644)) == -1)
    merror("Opening the group file.");

  if (file_size(group_file) == 0) {
    write_from(group_file, (char*)(&groups[0]), sizeof(group));
    next_group_id = 1;
  } else 
    populate_group_table_from_file(group_file);
}


void init_hash (void) {
  printf("Initializing hash stuff...\n");
  inhibit_file_write = 1;
  init_string_hash();
  init_group_hash();
  inhibit_file_write = 0;
  printf("Initializing hash stuff...done\n");
}

void flush_hash(void) {
  flush_groups(); 
}
