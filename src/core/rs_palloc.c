
#include <rs_config.h>
#include <rs_core.h>


static char *rs_pmemalloc(rs_pool_t *p, uint32_t size);
static int rs_palloc_grow(rs_pool_t *p, int id);
static int rs_palloc_new(rs_pool_t *p, int id);

static int rs_palloc_grow(rs_pool_t *p, int id)
{
    rs_pool_class_t *c;
    char            **t;
    uint32_t        size;

    c = &(p->slab_class[id]);

    /* no more slabs */
    if(c->used_slab == c->total_slab) {
        size = (c->total_slab == 0) ? 16 : c->total_slab * 2; 

        /* resize slabs, unchange old data */
        t = realloc(c->slab, size * sizeof(char *));

        if(t == NULL) {
            rs_log_err(rs_errno, "realloc(%u) failed", size * sizeof(char *));
            return RS_ERR;
        }

        c->total_slab = size;
        c->slab = t;
    }

    return RS_OK;
}

static int rs_palloc_new(rs_pool_t *p, int id)
{
    uint32_t        len;
    rs_pool_class_t *c;
    char            *t;

    t = NULL;
    c = &(p->slab_class[id]);
    len = c->size * c->num;

    /* add a new slabs */
    if(rs_palloc_grow(p, id) != RS_OK) {
        return RS_ERR;
    }

    /* allocate memory to new slab */
    if((t = rs_pmemalloc(p, len)) == NULL) {
        return RS_ERR;
    }

    rs_memzero(t, len);
    c->chunk = t;
    c->free = c->num;
    c->slab[c->used_slab++] = t;

    rs_log_debug(0, "id %d, used slab num = %u, total slab num = %u", 
            id,
            c->used_slab, 
            c->total_slab);

    return RS_OK;
}

static char *rs_pmemalloc(rs_pool_t *p, uint32_t size)
{
    char *t;

    t = NULL;

    if(p->flag == RS_POOL_PAGEALLOC) {
        /* no prelocate */
        if(p->cur_page++ >= p->max_page - 1 || size > p->free_size) 
        {
            rs_log_err(0, "rs_slab_allocmem() failed, no more memory");
            return NULL;
        }

        t = malloc(size);

        if(t == NULL) {
            rs_log_err(rs_errno, "malloc(%u) failed", size);
            return NULL;
        }

    } else if(p->flag == RS_POOL_PREALLOC) {
        /* prealloc */
        if(size > p->free_size) {
            rs_log_err(0, "rs_slab_allocmem() failed, no more memory");
            return NULL;
        }

        t = p->cur; 
        p->cur = p->cur + size;
    }

    p->free_size -= size;
    p->used_size += size;

    rs_log_debug(0, "pool free_size = %u, used_size = %u", 
            p->free_size, p->used_size);

    return t;
}

int rs_palloc_id(rs_pool_t *p, uint32_t size)
{
    int id;
    
    id = 0;

    if(size == 0) {
        rs_log_err(0, "rs_memslab_clsid() faield, size = 0");
        return RS_ERR;
    }

    /* if > 1MB use malloc() */
    if(size > RS_MEMSLAB_CHUNK_SIZE) {
        rs_log_debug(0, "palloc size %u overflow %u", 
                size, size - RS_MEMSLAB_CHUNK_SIZE); 
        return RS_SLAB_OVERFLOW;
    }

    while(size > p->slab_class[id].size) {
        /* reach the last index of class */
        if((uint32_t) id++ == p->max_class) {
            rs_log_debug(0, "palloc size %u overflow %u", 
                    size, size - RS_MEMSLAB_CHUNK_SIZE); 
            return RS_SLAB_OVERFLOW;
        }
    }

    rs_log_debug(0, "pool class size %u, clsid %d", size, id);

    return id;
}

rs_pool_t *rs_create_pool(uint32_t init_size, uint32_t mem_size, double factor, 
        int32_t flag)
{
    int         i;
    uint32_t    ps;
    rs_pool_t   *p;
    char        *t;

    mem_size = rs_align(mem_size, RS_ALIGNMENT); 
    init_size = rs_align(init_size, RS_ALIGNMENT); 

    rs_log_debug(0, "pool align mem_size %u", mem_size);

    if(flag == RS_POOL_PREALLOC) {
        /* prealloc memory */
        t = (char *) malloc(sizeof(rs_pool_t) + mem_size);

        if(t == NULL) {
            rs_log_err(rs_errno, "malloc(%u) failed", 
                    sizeof(rs_pool_t) + mem_size);
            return NULL;
        }

        p = (rs_pool_t *) t;
        p->start = t + sizeof(rs_pool_t);
        p->cur = p->start;

    } else if(flag == RS_POOL_PAGEALLOC) {
        /* pagealloc memory */
        ps = (mem_size / RS_MEMSLAB_CHUNK_SIZE) * sizeof(char *);
        t = (char *) malloc(sizeof(rs_pool_t) + ps);

        if(t == NULL) {
            rs_log_err(rs_errno, "malloc(%u) failed", 
                    sizeof(rs_pool_t) + ps);
            return NULL;
        }

        p = (rs_pool_t *) t;
        p->start = t + sizeof(rs_pool_t);
        rs_memzero(p->start, ps);
        p->cur_page = 0;
        p->max_page = mem_size / RS_MEMSLAB_CHUNK_SIZE;
    
    } else {
        rs_log_err(0, "unknown slab flag %d", flag);
        return NULL;
    }

    p->flag = flag;
    p->max_size = mem_size;
    p->free_size = mem_size;
    p->used_size = 0;
    rs_memzero(p->slab_class, sizeof(p->slab_class));

    for(i = 0; i < RS_MEMSLAB_CLASS_IDX_MAX && 
            init_size < RS_MEMSLAB_CHUNK_SIZE; i++) 
    {
        rs_log_debug(0, "pool align init_size %u", init_size);
        p->slab_class[i].size = init_size;
        p->slab_class[i].num = RS_MEMSLAB_CHUNK_SIZE / init_size;

        p->slab_class[i].chunk = NULL;
        p->slab_class[i].free_chunk = NULL;
        p->slab_class[i].slab = NULL;

        p->slab_class[i].used_slab = 0;
        p->slab_class[i].total_slab = 0;

        p->slab_class[i].free = 0;
        p->slab_class[i].total_free = 0;
        p->slab_class[i].used_free = 0;

        rs_log_debug(0, "slab class id= %d, chunk size = %u, num = %u",
                i, p->slab_class[i].size, p->slab_class[i].num);

        init_size = rs_align((uint32_t) (init_size * factor), RS_ALIGNMENT);
    }

    p->max_class = i;
    p->slab_class[i].size = RS_MEMSLAB_CHUNK_SIZE;
    p->slab_class[i].num = 1;

    return p;
}

char *rs_palloc(rs_pool_t *p, uint32_t size, int id)
{
    rs_pool_class_t *c;
    char            *t;

    t = NULL;
    c = NULL;

    // use malloc()
    if(id == RS_SLAB_OVERFLOW) {
        t = (char *) malloc(size);

        if(t == NULL) {
            rs_log_err(rs_errno, "malloc(%u) failed", size);
            return NULL;
        }

        return t;
    }

    c = &(p->slab_class[id]);

    if(!(c->chunk != NULL || c->used_free > 0 || rs_palloc_new(p, id) == RS_OK))
    {
        return NULL;
    } else if(c->used_free > 0) {
        t = c->free_chunk[--c->used_free];
        rs_log_debug(0, "used free chunk num %u", c->used_free);
    } else {
        t = c->chunk;

        if(--c->free > 0) {
            c->chunk = c->chunk + c->size;
        } else {
            c->chunk = NULL;
        }

        rs_log_debug(0, "free chunk num = %u", c->free);
    }

    return t;
}

void rs_pfree(rs_pool_t *p, char *data, int id)
{
    rs_pool_class_t *c;
    char            **free_chunk;
    uint32_t        tf;
    
    c = NULL;
    free_chunk = NULL;

    if(id == RS_SLAB_OVERFLOW) {
       free(data); 
       return;
    }

    c = &(p->slab_class[id]);

    if (c->used_free == c->total_free) {
        tf = (c->total_free == 0) ? 16 : c->total_free * 2;

        free_chunk = realloc(c->free_chunk, tf * sizeof(char *));

        if(free_chunk == NULL) {
            rs_log_err(rs_errno, "realloc(%u) failed", tf * sizeof(char *));
            return;
        }

        c->free_chunk = free_chunk;
        c->total_free = tf;
    }

    c->free_chunk[c->used_free++] = data;

    rs_log_debug(0, "used free chunk num = %u", c->used_free);
}

void rs_destroy_pool(rs_pool_t *p)
{
    uint32_t    i;
    char        *t;

    for(i = 0; i <= p->max_class; i++) {

        if(p->slab_class[i].slab != NULL) {
            free(p->slab_class[i].slab);
        }

        if(p->slab_class[i].free_chunk != NULL) {
            free(p->slab_class[i].free_chunk);
        }
    }

    if(p->flag == RS_POOL_PREALLOC) {
        free(p);
    } else if(p->flag == RS_POOL_PAGEALLOC) {
        for(i = 0; i < p->max_page; i++) {
            t = p->start + sizeof(char *);
            if(t != NULL) {
                free(t);
            }
        } 
    }
}
