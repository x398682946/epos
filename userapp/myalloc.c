/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 */
#include <sys/types.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <syscall.h>
#include <stdio.h>

struct chunk {
    char signature[4];  /* "OSEX" */
    struct chunk *next; /* ptr. to next chunk */
    int state;          /* 0 - free, 1 - used */
#define FREE   0
#define USED   1

    int size;           /* size of this chunk */
};

#define CHUNK_SIZE sizeof(struct chunk)
static struct chunk *chunk_head;

void *g_heap;
void *tlsf_create_with_pool(uint8_t *heap_base, size_t heap_size)
{
    chunk_head = (struct chunk *)heap_base;
    strncpy(chunk_head->signature, "OSEX", 4);
    chunk_head->next = NULL;
    chunk_head->state = FREE;
    chunk_head->size  = heap_size;

    return NULL;
}

void test()
{
    int i = 0;
    struct chunk *chunk_runner=chunk_head;
    printf("\r\n");
    while(chunk_runner)
    {
        printf("#%d\tstate:%d\taddress:0x%08X\tnext:0x%08X\tsize:%d\r\n", i++, chunk_runner->state, chunk_runner, chunk_runner->next, chunk_runner->size);
        chunk_runner = chunk_runner->next;
    }
}

// 线程安全
#define p(x) sem_wait(x)
#define v(x) sem_signal(x)

#define NOT_CREATE 0
int sem_mutex = NOT_CREATE;

int get_sem()
{
  if(sem_mutex == NOT_CREATE)
    sem_mutex = sem_create(1);

  return sem_mutex;
}

// 首次适应
void *malloc(size_t size)
{
    if (size == 0)
      return NULL;

    int sem_mutex = get_sem();
    p(sem_mutex);

    // 查找内存块
    struct chunk *chunk_runner = chunk_head;
    while (chunk_runner)
    {
        if (chunk_runner->state == FREE)
        {
            if(size == chunk_runner->size - CHUNK_SIZE)
            {
              chunk_runner->state = USED;
              break;
            }
            else if(size <= (chunk_runner->size - 2 * CHUNK_SIZE))
            {
                struct chunk *old_next = chunk_runner->next;
                int old_size = chunk_runner->size;

                chunk_runner->state = USED;
                chunk_runner->size = size + CHUNK_SIZE;
                chunk_runner->next = (struct chunk *)((uint8_t *)chunk_runner + CHUNK_SIZE + size);
                strncpy(chunk_runner->signature, "OSEX", 4);

                chunk_runner->next->state = FREE;
                chunk_runner->next->size = old_size - CHUNK_SIZE - size;
                chunk_runner->next->next = old_next;
                strncpy(chunk_runner->next->signature, "OSEX", 4);
                break;
            }
        }
      chunk_runner = chunk_runner->next;
    }
    v(sem_mutex);

    return (void *)((uint8_t *)chunk_runner + CHUNK_SIZE);
}

void free(void *ptr)
{
    if(!ptr)
      return;

    // 释放内存
    struct chunk *achunk=(struct chunk *)(((uint8_t *)ptr)-CHUNK_SIZE);
    if(strncmp(achunk->signature, "OSEX", 4) == 0)
      achunk->state = FREE;

    // 合并空闲块
    int i=0;
    struct chunk *chunk_runner = chunk_head;
    while(chunk_runner)
    {
      if (!chunk_runner->next)
        break;
      
      if (chunk_runner->state == FREE && chunk_runner->next->state == FREE)
      {
        chunk_runner->size = chunk_runner->size + chunk_runner->next->size;
        chunk_runner->next = chunk_runner->next->next;
      }
      else
      {
        chunk_runner = chunk_runner->next;
      }
      i++;
    }
}

void *calloc(size_t num, size_t size)
{
    uint8_t* ptr = malloc(num * size);
    memset(ptr, 0, num * size);
    return (void *)ptr;
}

void *realloc(void *oldptr, size_t size)
{
    if(!oldptr)
      return malloc(size);
    
    if(size == 0)
    {
      free(oldptr);
      return NULL;
    }

    free(oldptr);
    uint8_t *newptr = malloc(size);
    memcpy(newptr, oldptr, ((struct chunk *)((uint8_t *)oldptr - CHUNK_SIZE))->size);

    return newptr;
}

/*************D O  N O T  T O U C H  A N Y T H I N G  B E L O W*************/
static void tsk_malloc(void *pv)
{
  int i, c = (int)pv;
  char **a = malloc(c*sizeof(char *));
  for(i = 0; i < c; i++) {
	  a[i]=malloc(i+1);
	  a[i][i]=17;
  }
  for(i = 0; i < c; i++) {
	  free(a[i]);
  }
  free(a);

  task_exit(0);
}

#define MESSAGE(foo) printf("%s, line %d: %s", __FILE__, __LINE__, foo)
void test_allocator()
{
  char *p, *q, *t;

  MESSAGE("[1] Test malloc/free for unusual situations\r\n");

  MESSAGE("  [1.1]  Allocate small block ... ");
  p = malloc(17);
  if (p == NULL) {
    printf("FAILED\r\n");
	return;
  }
  p[0] = p[16] = 17;
  printf("PASSED\r\n");

  MESSAGE("  [1.2]  Allocate big block ... ");
  q = malloc(4711);
  if (q == NULL) {
    printf("FAILED\r\n");
	return;
  }
  q[4710] = 47;
  printf("PASSED\r\n");

  MESSAGE("  [1.3]  Free big block ... ");
  free(q);
  printf("PASSED\r\n");

  MESSAGE("  [1.4]  Free small block ... ");
  free(p);
  printf("PASSED\r\n");

  MESSAGE("  [1.5]  Allocate huge block ... ");
  q = malloc(32*1024*1024-sizeof(struct chunk));
  if (q == NULL) {
    printf("FAILED\r\n");
	return;
  }
  q[32*1024*1024-sizeof(struct chunk)-1]=17;
  free(q);
  printf("PASSED\r\n");

  MESSAGE("  [1.6]  Allocate zero bytes ... ");
  if ((p = malloc(0)) != NULL) {
    printf("FAILED\r\n");
	return;
  }
  printf("PASSED\r\n");

  MESSAGE("  [1.7]  Free NULL ... ");
  free(p);
  printf("PASSED\r\n");

  MESSAGE("  [1.8]  Free non-allocated-via-malloc block ... ");
  int arr[5] = {0x55aa4711,0x5a5a1147,0xa5a51471,0xaa551741,0x5aa54171};
  free(&arr[4]);
  if(arr[0] == 0x55aa4711 &&
     arr[1] == 0x5a5a1147 &&
	 arr[2] == 0xa5a51471 &&
	 arr[3] == 0xaa551741 &&
	 arr[4] == 0x5aa54171) {
	  printf("PASSED\r\n");
  } else {
	  printf("FAILED\r\n");
	  return;
  }

  MESSAGE("  [1.9]  Various allocation pattern ... ");
  int i;
  size_t pagesize = sysconf(_SC_PAGESIZE);
  for(i = 0; i < 7411; i++){
    p = malloc(pagesize);
	p[pagesize-1]=17;
    q = malloc(pagesize * 2 + 1);
	q[pagesize*2]=17;
    t = malloc(1);
	t[0]=17;
    free(p);
    free(q);
    free(t);
  }

  char **a = malloc(2741*sizeof(char *));
  for(i = 0; i < 2741; i++) {
	  a[i]=malloc(i+1);
	  a[i][i]=17;
  }
  for(i = 0; i < 2741; i++) {
	  free(a[i]);
  }
  free(a);

  if(chunk_head->next != NULL || chunk_head->size != 32*1024*1024) {
	printf("FAILED\r\n");
	return;
  }
  printf("PASSED\r\n");

  MESSAGE("  [1.10] Allocate using calloc ... ");
  int *x = calloc(17, 4);
  for(i = 0; i < 17; i++)
	  if(x[i] != 0) {
		  printf("FAILED\r\n");
		  return;
	  } else
	      x[i] = i;
  free(x);
  printf("PASSED\r\n");

  MESSAGE("[2] Test realloc() for unusual situations\r\n");

  MESSAGE("  [2.1]  Allocate 17 bytes by realloc(NULL, 17) ... ");
  p = realloc(NULL, 17);
  if (p == NULL) {
    printf("FAILED\r\n");
	return;
  }
  p[0] = p[16] = 17;
  printf("PASSED\r\n");
  MESSAGE("  [2.2]  Increase size by realloc(., 4711) ... ");
  p = realloc(p, 4711);
  if (p == NULL) {
    printf("FAILED\r\n");
	return;
  }
  if ( p[0] != 17 || p[16] != 17 ) {
    printf("FAILED\r\n");
	return;
  }
  p[4710] = 47;
  printf("PASSED\r\n");

  MESSAGE("  [2.3]  Decrease size by realloc(., 17) ... ");
  p = realloc(p, 17);
  if (p == NULL) {
    printf("FAILED\r\n");
	return;
  }
  if ( p[0] != 17 || p[16] != 17 ) {
    printf("FAILED\r\n");
	return;
  }
  printf("PASSED\r\n");

  MESSAGE("  [2.4]  Free block by realloc(., 0) ... ");
  p = realloc(p, 0);
  if (p != NULL) {
	printf("FAILED\r\n");
    return;
  } else
	printf("PASSED\r\n");

  MESSAGE("  [2.5]  Free block by realloc(NULL, 0) ... ");
  p = realloc(realloc(NULL, 0), 0);
  if (p != NULL) {
    printf("FAILED\r\n");
    return;
  } else
	printf("PASSED\r\n");

  MESSAGE("[3] Test malloc/free for thread-safe ... ");

  int t1, t2;
  char *s1 = malloc(1024*1024),
       *s2 = malloc(1024*1024);
  t1=task_create(s1+1024*1024, tsk_malloc, (void *)5000);
  t2=task_create(s2+1024*1024, tsk_malloc, (void *)5000);
  task_wait(t1, NULL);
  task_wait(t2, NULL);
  free(s1);
  free(s2);

  if(chunk_head->next != NULL || chunk_head->size != 32*1024*1024) {
	printf("FAILED\r\n");
	return;
  }
  printf("PASSED\r\n");
}
/*************D O  N O T  T O U C H  A N Y T H I N G  A B O V E*************/