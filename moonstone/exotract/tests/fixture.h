#ifndef EXOTRACT_FIXTURE_H
#define EXOTRACT_FIXTURE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct et_counter et_counter;
struct et_counter { int count; };
et_counter *et_create(void);
void et_destroy(et_counter *ptr);
int et_add(et_counter *ptr,int value);
#ifdef __cplusplus
}
#endif
#endif
#if defined(EXOTRACT_IMPL_FIXTURE) && !defined(EXOTRACT_FIXTURE_IMPLEMENTED)
#define EXOTRACT_FIXTURE_IMPLEMENTED
#include <stdlib.h>
et_counter *et_create(void){return (et_counter*)calloc(1,sizeof(et_counter));}
void et_destroy(et_counter *ptr){free(ptr);}
int et_add(et_counter *ptr,int value){if(!ptr)return -1;return ptr->count+=value;}
#endif
