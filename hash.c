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
#include "input.h"

static char *string_storage = NULL;
static int string_storage_length = INITIAL_STRING_STORAGE_LENGTH;
int next_string = 0;
static int string_storage_file = 0;

static int *string_storage_table = NULL;
static int string_storage_table_length = STRING_STORAGE_TABLE_LENGTH;

static int *group_table = NULL;
static int group_table_length = GROUP_TABLE_LENGTH;
static int next_group_id = 0;
static int group_file = 0;

static int *node_table = NULL;
static int node_table_length = INITIAL_NODE_TABLE_LENGTH;
unsigned int previous_instance_node = 0;

static int inhibit_file_write = 0;

#define HASH_GRANULARITY 1024

/* one_at_a_time_hash() */

unsigned int hash(const char *key, unsigned int len, 
		  unsigned int table_length) {
  unsigned int hash, i;
  for (hash=0, i=0; i<len; ++i) {
    hash += key[i];
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return (hash & (table_length - 1));
} 

char *get_string(int offset) {
  // FIXME
  wash_string(string_storage + offset);
  return string_storage + offset;
}

void extend_string_storage(void) {
  int new_length = string_storage_length * 2;
  char *new_string_storage = cmalloc(new_length);
  
#ifdef USAGE
  printf("Extending string storage from %dM to %dM\n", 
	 meg(string_storage_length), meg(new_length));
#endif
  memcpy(new_string_storage, string_storage, string_storage_length);
  crfree(string_storage, string_storage_length);
  string_storage = new_string_storage;
  string_storage_length = new_length;
}

unsigned int enter_string_storage(const char *string) {
  int string_length = strlen(string);
  int search = hash(string, string_length, string_storage_table_length);
  int offset;

#if 0
  int candidate = search;
#endif

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
    if (next_string + string_length >= string_storage_length)
      extend_string_storage();
    strcpy((string_storage + next_string), string);
    if (! inhibit_file_write &&
	! inhibit_file_writes) 
      write(string_storage_file, string, string_length + 1);
    string_storage_table[search] = next_string;
    offset = next_string;
    next_string += string_length + 1;
  } 
#if 0
  if (search - candidate > 5)
    printf("%s => %d ... %d\n", string, candidate, search-candidate);
#endif

  return offset;
}

unsigned int initial_enter_string_storage(const char *string) {
  int string_length = strlen(string);
  int search = hash(string, string_length, string_storage_table_length);
  int offset;

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
    string_storage_table[search] = next_string;
    next_string += string_length + 1;
    offset = next_string;
  } 

  return offset;
}

void flush_strings(void) {
  if (lseek64(string_storage_file, (loff_t)0, SEEK_SET) < 0) {
    printf("Trying to see to %d\n", 0);
    merror("Seeking the string storage file");
  }
  write_from(string_storage_file, string_storage, next_string);
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

void smart_populate_string_table_from_file(int fd) {
  loff_t fsize = file_size(fd);
  int offset = 0;

  read_block(string_storage_file, string_storage, fsize);
  while (offset < fsize) {
    // FIXME; remove.
    wash_string(string_storage + offset);
    //offset = initial_enter_string_storage(string_storage + offset);
    initial_enter_string_storage(string_storage + offset);
    offset += strlen(string_storage + offset) + 1;
  }
}

void init_string_hash (void) {
  string_storage = cmalloc(string_storage_length);
  string_storage_table =
    (int*)cmalloc(STRING_STORAGE_TABLE_LENGTH * sizeof(int));

#ifdef USAGE
  printf("Allocating %dM for string storage\n", meg(string_storage_length));
  printf("Allocating %dM for string hash table\n",
	 meg(STRING_STORAGE_TABLE_LENGTH * sizeof(int)));
#endif

  if ((string_storage_file = open64(index_file_name(STRING_STORAGE_FILE),
			      O_RDWR|O_CREAT, 0644)) == -1)
    merror("Opening the string storage file.");

  if (file_size(string_storage_file) == 0) {
    write_from(string_storage_file, "@@@", 4);
    next_string = 4;
  } else 
    smart_populate_string_table_from_file(string_storage_file);
}


/*** Nodes ***/

node *allocate_new_node(const char *message_id, unsigned int group_id, 
			int search) {
  int id = next_id();
  node *nnode = &nodes[id];

#if 0
  printf("Getting new node %s %d\n", message_id, id);
#endif

  node_table[search] = id;

  if (! inhibit_file_write) {
    nnode->id = id;
    nnode->group_id = group_id;
    nnode->message_id = enter_string_storage(message_id);
  }
  return nnode;
}

node *get_node(const char *message_id, unsigned int group_id) {
  int string_length = strlen(message_id);
  int search = hash(message_id, string_length, node_table_length);
  int offset = 0;
  node *g;

#if 0
  printf("Entering node '%s', %d\n", message_id, group_id);
#endif

  previous_instance_node = 0;

  while (1) {
    offset = node_table[search];
    if (! offset)
      break;
    else if (offset && ! strcmp(message_id,
				get_string(nodes[offset].message_id)))
      break;
    if (search++ >= node_table_length)
      search = 0;
  }

  if (! offset) 
    return allocate_new_node(message_id, group_id, search);
  else {
    g = &nodes[offset];
    while (1) {
      if (g->group_id == group_id)
	return g;
      if (g->next_instance == 0)
	break;
      previous_instance_node = g->id;
      g = &nodes[g->next_instance];
    } 
    return allocate_new_node(message_id, group_id, search);
  }
}

void hash_node(const char *message_id, unsigned int node_id) {
  int string_length = strlen(message_id);
  int search = hash(message_id, string_length, node_table_length);
  int offset = 0;

  while (1) {
    offset = node_table[search];
    if (! offset)
      break;
    else if (offset && ! strcmp(message_id,
				get_string(nodes[offset].message_id)))
      break;
    if (search++ >= node_table_length)
      search = 0;
  }

  if (! offset) 
    node_table[search] = node_id;
}

node *find_node(const char *message_id) {
  int string_length = strlen(message_id);
  int search = hash(message_id, string_length, node_table_length);
  int offset = 0;

  while (1) {
    offset = node_table[search];
    if (! offset)
      break;
    else if (offset && ! strcmp(message_id,
				get_string(nodes[offset].message_id)))
      break;
    if (search++ >= node_table_length)
      search = 0;
  }

  if (! offset) 
    return NULL;
  else
    return &nodes[offset];
}

void init_node_table(void) {
  node_table = (int*)cmalloc(node_table_length * sizeof(int));
#ifdef USAGE
  printf("Allocating %dM for node hash table\n",
	 meg(node_table_length * sizeof(int)));
#endif
}

/*** Groups ***/

group *get_group_1(const char *group_name, int create_new_group) {
  int string_length = strlen(group_name);
  int search = hash(group_name, string_length, group_table_length);
  int offset, group_id;
  group *g;

#ifdef DEBUG
  printf("Entering group '%s'\n", group_name);
#endif

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
    if (! create_new_group)
      return NULL;

    group_id = next_group_id++;
    if (! inhibit_file_write) {
      g = &groups[group_id];
      g->group_name = enter_string_storage(group_name);
      g->group_description = 0;
      g->group_id = group_id;
      g->dirtyp = 1;
    }
    offset = group_id;
    group_table[search] = group_id;
  } 

  return &groups[offset];
}

group *get_group(const char *group_name) {
  return get_group_1(group_name, 1);
}

group *find_group(const char *group_name) {
  return get_group_1(group_name, 0);
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
  group *g;

  for (i = 0; i<fsize/sizeof(group); i++) {
    read_block(fd, (char*)(&groups[i]), sizeof(group));
    g = &groups[i];
    if (g->group_name) {
      g->numeric_nodes = NULL;
      g->thread_nodes = NULL;
      g->nodes_length = 0;
      get_group(get_string(g->group_name));
#ifdef DEBUG
      printf("%s (%d, %d)\n", get_string(g->group_name), g->total_articles,
	     g->max_article);
#endif
    }
  }
}

void init_group_hash (void) {
  group_table = (int*)cmalloc(MAX_GROUPS * sizeof(int));
#ifdef USAGE
  printf("Allocating %dM for group hash table\n",
	 meg(MAX_GROUPS * sizeof(int)));
#endif

  if ((group_file = open64(index_file_name(GROUP_FILE),
			   O_RDWR|O_CREAT, 0644)) == -1)
    merror("Opening the group file.");

  next_group_id = 1;

  if (file_size(group_file) == 0) {
    write_from(group_file, (char*)(&groups[0]), sizeof(group));
  } else 
    populate_group_table_from_file(group_file);
}


void init_hash (void) {
  inhibit_file_write = 1;
  init_string_hash();
  init_group_hash();
  init_node_table();
  inhibit_file_write = 0;
}

void flush_hash(void) {
  flush_groups(); 
}

