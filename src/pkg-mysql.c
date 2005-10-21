/*---------------------------------------------------------------------------
 * mySQL Support Efuns.
 * Original code written 1999 by Mark Daniel Reidel.
 *
 *---------------------------------------------------------------------------
 * This file holds the efuns interfacing with mySQL. See the file INSTALL
 * or doc/concepts/mysql for setup instructions.
 *
 *   efun: db_connect()
 *   efun: db_close()
 *   efun: db_exec()
 *   efun: db_fetch()
 *   efun: db_affected_rows()
 *   efun: db_conv_string()
 *   efun: db_handles()
 *
 *---------------------------------------------------------------------------
 */

#include "driver.h"

#ifdef USE_MYSQL

#include "typedefs.h"

#include "my-alloca.h"
#include <stddef.h>
#include <string.h>

#include <mysql/mysql.h>
#include <mysql/errmsg.h>

#include "pkg-mysql.h"

#include "array.h"
#include "interpret.h"
#include "svalue.h"
#include "xalloc.h"

/*-------------------------------------------------------------------------*/

typedef struct db_dat_s db_dat_t;

/*--- struct db_dat_s: SQL connection handle --- */
struct db_dat_s
{
    db_dat_t  *next;
    db_dat_t  *prev;
    MYSQL     *mysql_dat;
    MYSQL_RES *mysql_result;
    MYSQL_ROW  mysql_row;
};

static db_dat_t *my_dat;
  /* List of connection handles, newest first.
   */

static void raise_db_error (db_dat_t *dat) NORETURN;

/*-------------------------------------------------------------------------*/
static db_dat_t *
allocate_new_dat(void)

/* Allocate a free handle to use for a connection to an SQL-server
 * and return it. The handle is also chained into the global list my_dat
 * at its beginning.
 */

{
    db_dat_t *tmp;

    if ( !my_dat ) /* The chained list has not been allocated */
    {
        my_dat = pxalloc(sizeof(*my_dat));
       if ( !my_dat )
       {
           error("Out of memory.\n");
           /* NOTREACHED */
           return NULL;
       }
        my_dat->prev = NULL;
        my_dat->next = NULL;
        my_dat->mysql_dat = NULL;
        my_dat->mysql_result = NULL;
        return my_dat;
    }

    /* The chained list exists */
    
    tmp = my_dat->prev = pxalloc(sizeof(db_dat_t));
    if ( !tmp )
    {
       error("Out of memory.\n");
       return NULL;
    }
    tmp->next = my_dat; /* Put the new handle to the beginning */
    tmp->prev = NULL;
    tmp->mysql_dat = NULL;
    tmp->mysql_result = NULL;
    my_dat = tmp;

    return my_dat;
} /* allocate_new_dat() */

/*-------------------------------------------------------------------------*/
static db_dat_t *
find_dat_by_handle (unsigned int i)

/* Return the corresponding db_dat_t-structure for the handle with id <i>.
 * When the structure has been found, it is moved to the beginning
 * of the chained list as it is VERY likely that the next operation
 * will also be performed on this handle.
 * If the handle was not found, NULL is returned.
 */

{
    db_dat_t *tmp, *tmp2, *tmp3;
    unsigned int id;

    if ( !my_dat )
        return NULL;

    tmp = my_dat;
    while (   ((id = tmp->mysql_dat->thread_id) != i)
           && (tmp = tmp->next) )
       NOOP;

    if ( id != i ) // handle NOT found
        return NULL;

    /* Put the selected pointer at the beginning */
    if ( tmp == my_dat )
        return tmp;

    tmp2 = tmp->prev;
    tmp3 = tmp->next;
    tmp2->next = tmp3;
    if ( tmp3 )
        tmp3->prev = tmp2;
    my_dat->prev = tmp;
    tmp->prev = NULL;
    tmp->next = my_dat;

    /* Point my_dat at the first entry again */
    my_dat = tmp;

    return tmp;
} /* find_dat_by_handle() */

/*-------------------------------------------------------------------------*/
static unsigned int
remove_dat (db_dat_t *dat)

/* Remove the database-handle from memore and the chained list.
 * Also clean any memory allocated for SQL-use.
 * The result is the handle of the data-connection that was
 * closed, or 0 if the handle was not found.
 */

{
    db_dat_t *tmp, *tmp2;
    unsigned int i;

    if ( !dat )
        return 0;

    /* Close the database connection */
    if ( dat->mysql_dat )
    {
        if (dat->mysql_result)
            mysql_free_result(dat->mysql_result);
        i = dat->mysql_dat->thread_id;
        mysql_close(dat->mysql_dat);
    }

    /* Unlink the structure from the list */
    tmp = dat->prev;
    tmp2 = dat->next;
    if ( tmp )
        tmp->next = tmp2;
    if ( tmp2 )
    {
        if ( !tmp )
            my_dat = tmp2;
        tmp2->prev = tmp;
    }
    if ( dat == my_dat )
        my_dat = NULL;

    pfree(dat);
    return i;
} /* remove_dat() */

/*-------------------------------------------------------------------------*/
static unsigned int
remove_dat_by_handle (int i)

/* Remove the memory for the handle with the number <handle> from
 * the memory and return the handle.
 */

{
    db_dat_t *tmp;

    tmp = find_dat_by_handle(i);
    return remove_dat(tmp);
} /* remove_dat_by_handle() */

/*-------------------------------------------------------------------------*/
static void
raise_db_error (db_dat_t *dat)

/* Raise an error according to the last operation on the passed
 * SQL-connection. The connection is closed.
 */

{
    char *tmp;
    char *err_string;

    if ( !dat )
    {
        error( "An unknown error occured during the current database-"
               "operation\n");
        /* NOTREACHED */
       abort();
    }
    tmp = mysql_error(dat->mysql_dat);
    err_string = alloca(strlen(tmp) + 2);
    strcpy(err_string, tmp);
    strcat(err_string, "\n");
    remove_dat(dat);
    error(err_string);
    /* NOTREACHED */
    abort();
} /* raise_db_error() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_db_affected_rows (svalue_t *sp)

/* EFUN db_affected_rows()
 *
 *   int db_affected_rows(int handle)
 *
 * Return the number of affected rows of the last SQL-statement that
 * has been sent to the SQL-server via handle <handle>.
 * Only useful for DELETE- or UPDATE-operations.
 */

{
    db_dat_t    *dat;
    int          rows;
    unsigned int handle;

    handle = (unsigned int)sp->u.number;
    if ( !(dat = find_dat_by_handle(handle)) )
        error("Illegal handle for database.\n");
    rows = mysql_affected_rows(dat->mysql_dat);
    free_svalue(sp); /* Well, it's just a number */
    put_number(sp, rows);
    return sp;
} /* f_db_affected_rows() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_db_conv_string (svalue_t *sp)

/* EFUN db_conv_string()
 *
 *   string db_conv_string(string str)
 *
 * Convert the string <str> into a string that is correctly interpretated
 * for usage as a string in db_exec(), e.g. ' is replaced with \' and so
 * on.
 */

{
    char *s;
    char *buff;

    s = sp->u.string;
    buff = alloca(strlen(s)*2 +1);
    if ( !buff )
    {
        error("Out of memory.\n");
        /* NOTREACHED */
        return sp;
    }
    mysql_escape_string(buff, s, strlen(s));

    /* Re-allocate the escaped string to its proper length */
    s = string_copy(buff);

    free_string_svalue(sp);
    put_malloced_string(sp, s);
    return sp;
} /* f_db_conv_string() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_db_close (svalue_t *sp)

/* EFUN db_close()
 *
 *   int db_close(int handle)
 *
 * Close the server-connection with the handle <handle>
 * Return the handle-number on success.
 */

{
    p_int     handle;
    db_dat_t *dat;

    handle = sp->u.number;
    if ( !(dat = find_dat_by_handle((unsigned int)handle)) )
    {
        error("Illegal handle for database.\n");
        /* NOTREACHED */
        return sp;
    }
    handle = (p_int)remove_dat(dat);
    free_svalue(sp); /* Well, it's just a number */
    put_number(sp, handle);
    return sp;
} /* db_close() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_db_connect (svalue_t *sp)

/* EFUN db_connect()
 *
 *   int db_connect(string database)
 *
 * Connect to the database <database> on the local mySQL-server.
 * The return-value is the handle for this connection.
 * If the database does not exist or the server is NOT started,
 * a runtime-error is raised.
 */

{
    char     *database;
    p_int     sock;
    db_dat_t *tmp;

    database = sp->u.string;

    tmp = allocate_new_dat();
    if ( !tmp )
    {
        error("Out of memory.\n");
        /* NOTREACHED */
        return NULL;
    }

    tmp->mysql_dat = mysql_init(0);
    if ( !tmp->mysql_dat )
    {
        remove_dat(tmp);
        error("Out of memory.\n");
        /* NOTREACHED */
        return NULL;
    }

    /* Only connections to LOCALHOST are currently possible
     * I wouldn't dare to implement synchronous DB-access via
     * TCP (that's something for ERQ wizards :-).
     */
    if ( !mysql_real_connect(tmp->mysql_dat, "localhost", 0, 0,
                             database, 0, 0, 0))
    {
        raise_db_error(tmp);
        /* NOTREACHED */
        return sp;
    }

    free_string_svalue(sp);
    sock = (signed)tmp->mysql_dat->thread_id;
    put_number(sp, sock);
    return sp;
} /* f_db_connect() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_db_exec (svalue_t *sp)

/* EFUN db_exec()
 *
 *   int db_exec(int handle, string statement)
 *
 * Execute the SQL-statement <statement> for the connection <handle> to
 * the SQL-server. The result is the handle if all went okay. If there
 * was an error in the statement, 0 is returned.
 */

{
    char         *s;
    db_dat_t     *dat;
    unsigned int  handle;
    unsigned int  err_no;

    handle = (unsigned int)sp[-1].u.number;
    s = sp->u.string;

    if ( !(dat = find_dat_by_handle(handle)) )
    {
        error("Illegal handle for database.\n");
        /* NOTREACHED */
        return sp;
    }

    if ( mysql_query(dat->mysql_dat, s) )
    {
        /* either a REAL error occured or just an error in the SQL-statement
         */

        err_no = mysql_errno(dat->mysql_dat);
        if (   (err_no == CR_COMMANDS_OUT_OF_SYNC)
            || (err_no == CR_SERVER_GONE_ERROR)
            || (err_no == CR_SERVER_LOST)
            || (err_no == CR_UNKNOWN_ERROR) )
        {
            /* A REAL error occured */
            raise_db_error(dat);
            return sp;
        }

        /* Just an error in the SQL-statement */
        free_string_svalue(sp);
        sp--;
        free_svalue(sp); /* Only a number */
        put_number(sp, 0);
        return sp;
    }

    /* If we used a select-statement, how many columns are returned? */
    
    if ( mysql_field_count(dat->mysql_dat) )
    {
        /* Try to initiate a row-by-row transfer */
        if ( !(dat->mysql_result = mysql_use_result(dat->mysql_dat)) )
        {
            raise_db_error(dat);
            /* NOTREACHED */
            return sp;
        }
    }

    free_string_svalue(sp);
    sp--;
    free_svalue(sp); /* Only a number */
    put_number(sp, (signed)handle);
    return sp;
} /* f_db_exec() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_db_fetch (svalue_t *sp)

/* EFUN db_fetch()
 *
 *   mixed db_fetch(int handle)
 *
 * Retrieve _ONE_ line of result of the latest SQL-action to the server
 * based on the handle <handle>. If not more results are on the server,
 * 0 is returned.
 */

{
    db_dat_t     *dat;
    vector_t     *v;
    int           num_cols, i;
    unsigned int  handle;

    handle = (unsigned int)sp->u.number;
    if ( !(dat = find_dat_by_handle(handle)) )
    {
        error("Illegal handle for database.\n");
        /* NOTREACHED */
        return sp;
    }

    if (!dat->mysql_result) 
    {
       free_svalue(sp);
       put_number(sp, 0);
       return sp;
    }

    /* Store the (next) row of the result in dat->mysql_row */

    dat->mysql_row = mysql_fetch_row(dat->mysql_result);
    if ( dat->mysql_row == NULL )
    {
        /* No more rows to fetch */
        free_svalue(sp); /* It's a number */
        put_number(sp, 0);
        return sp;
    }

    /* How many columns does every line contain? */
    
    num_cols = mysql_num_fields(dat->mysql_result);
    v = allocate_array(num_cols);
    if (!v)
    {
        error("Out of memory.\n");
        /* NOTREACHED */
        return sp;
    }
    
    for (i = 0; i < num_cols; i++)
        if (dat->mysql_row[i])
            put_malloced_string(v->item+i, string_copy(dat->mysql_row[i]));
        /* else return 0 for that entry */

    free_svalue(sp); /* It's a number */
    put_array(sp, v);
    return sp;
} /* f_db_exec() */

/*-------------------------------------------------------------------------*/
svalue_t *
f_db_handles (svalue_t *sp)

/* EFUN db_handles()
 *
 *   int *db_handles()
 *
 * Returns an array with all open handles to the SQL-server.
 * As mySQL is most of the time limited to 100 connections, you
 * should not let this number grow too big. The handles are sorted
 * in a special order: The last used handle is the first one and
 * the handle that hasn't been used for the longest time is
 * the last one. If no handles are open, an empty array is returned.
 */

{
    int elems;
    int i;
    db_dat_t *tmp;
    vector_t *v;

    tmp = my_dat;

    /* Maybe there's no open connection yet/anymore */
    if ( !tmp )
    {
        v = allocate_array(0);
        if (!v)
        {
            error("Out of memory.\n");
            /* NOTREACHED */
            return sp;
        }
        sp++;
        put_array(sp, v);
        return sp;
    }

    elems = 1;

    /* Count how many handles there are */
    while (tmp = tmp->next)
        elems++;

   /* Allocate an array to store all handle-ids */
    v = allocate_array(elems);
    if (!v)
    {
        error("Out of memory.\n");
        /* NOTREACHED */
        return sp;
    }

    /* Now browse through all handles again and store their ids */
    tmp = my_dat;
    for (i = 0; i < elems; i++)
    {
        put_number(v->item+i, tmp->mysql_dat->thread_id);
        tmp = tmp->next;
    }

    sp++;
    put_array(sp, v);
    return sp;
} /* f_db_handles() */

#endif /* USE_MYSQL */

/***************************************************************************/

