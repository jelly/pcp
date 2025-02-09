/*
 * Copyright (c) 2012-2014,2016,2019 Red Hat.
 * Copyright (c) 2011 Aconex.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#include "linux.h"
#include "filesys.h"
#include "interrupts.h"
#include <sys/stat.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#include <ctype.h>

#define MAX_INTERRUPT_LINES	1023

typedef struct {
    unsigned int	id;		/* becomes PMID item number */
    char		*name;		/* becomes PMNS sub-component */
    char		*text;		/* one-line metric help text */
    unsigned long	*values;	/* per-CPU values for this counter */
    unsigned long long	total;		/* aggregation of interrupt counts */
} interrupt_t;

typedef struct {
    unsigned int	cpuid;		/* CPU identifier */
    unsigned long long	count;		/* per-CPU sum of interrupt counts */
} online_cpu_t;

static char *iobuf;			/* buffer sized based on CPU count */
static unsigned int iobufsz;

static unsigned int cpu_count;
static online_cpu_t *online_cpumap;	/* maps input columns to CPU info */
static unsigned int lines_count;
static interrupt_t *interrupt_lines;
static unsigned int other_count;
static interrupt_t *interrupt_other;
static unsigned int softirqs_count;
static interrupt_t *softirqs;

static unsigned int refresh_interrupt_count;
static unsigned int refresh_softirqs_count;
static pmdaNameSpace *interrupt_tree;
static pmdaNameSpace *softirqs_tree;
unsigned int irq_err_count;

/*
 * One-shot initialisation for global interrupt-metric-related state
 */
static int
setup_interrupts(int reset)
{
    online_cpu_t *op;
    static int setup;

    if (!setup) {
	pmdaCacheOp(INDOM(INTERRUPT_NAMES_INDOM), PMDA_CACHE_LOAD);
	pmdaCacheOp(INDOM(SOFTIRQS_NAMES_INDOM), PMDA_CACHE_LOAD);
	pmdaCacheOp(INDOM(INTERRUPTS_INDOM), PMDA_CACHE_LOAD);
	pmdaCacheOp(INDOM(SOFTIRQS_INDOM), PMDA_CACHE_LOAD);
	if ((iobufsz = (_pm_ncpus * 64)) < BUFSIZ)
	    iobufsz = BUFSIZ;
	if ((iobuf = malloc(iobufsz)) == NULL)
	    return -oserror();
	setup = 1;
    }
    if (cpu_count != _pm_ncpus) {
	op = realloc(online_cpumap, _pm_ncpus * sizeof(online_cpu_t));
	if (!op)
	    return -oserror();
	online_cpumap = op;
	cpu_count = _pm_ncpus;
    }
    if (reset)
	memset(online_cpumap, 0, cpu_count * sizeof(online_cpu_t));
    return 0;
}

static char *
dynamic_name_lookup(unsigned int item, int cache)
{
    char *name;
    pmInDom dict = INDOM(cache);

    if (pmdaCacheLookup(dict, item, &name, NULL) == PMDA_CACHE_ACTIVE)
	return name;
    return NULL;
}

static interrupt_t *
dynamic_data_lookup(unsigned int item, int cache)
{
    pmInDom dict = INDOM(cache);
    void *data;
    char *name;

    if (pmdaCacheLookup(dict, item, &name, &data) == PMDA_CACHE_ACTIVE)
	return (interrupt_t *)data;
    return NULL;
}

static int
dynamic_item_lookup(const char *name, int cache)
{
    pmInDom dict = INDOM(cache);
    int inst;

    if (pmdaCacheLookupName(dict, name, &inst, NULL) == PMDA_CACHE_ACTIVE)
	return inst;
    return -1;
}

static unsigned int
dynamic_name_insert(const char *name, int cache, void *data)
{
    return pmdaCacheStore(INDOM(cache), PMDA_CACHE_ADD, name, data);
}

static void
dynamic_name_save(int cache, interrupt_t *data, int count)
{
    int i;
    pmInDom dict = INDOM(cache);

    for (i = 0; i < count; i++) {
	interrupt_t *ip = &data[i];
	pmdaCacheStore(dict, PMDA_CACHE_ADD, ip->name, (void *)ip);
    }
    pmdaCacheOp(dict, PMDA_CACHE_SAVE);
}

static void
update_lines_pmns(int domain, unsigned int item, unsigned int id)
{
    char entry[128];
    pmID pmid;

    if (item > MAX_INTERRUPT_LINES)
	return;

    pmid = pmID_build(domain, CLUSTER_INTERRUPT_LINES, item);
    pmsprintf(entry, sizeof(entry), "kernel.percpu.interrupts.line%d", id);
    pmdaTreeInsert(interrupt_tree, pmid, entry);
}

static void
update_other_pmns(int domain, const char *name)
{
    char entry[128];
    unsigned int item = dynamic_item_lookup(name, INTERRUPT_NAMES_INDOM);
    pmID pmid = pmID_build(domain, CLUSTER_INTERRUPT_OTHER, item);

    pmsprintf(entry, sizeof(entry), "%s.%s", "kernel.percpu.interrupts", name);
    pmdaTreeInsert(interrupt_tree, pmid, entry);
}

static pmdaNameSpace *
noop_interrupts_pmns(int domain)
{
    char entry[128];
    pmID pmid;

    pmid = pmID_build(domain, CLUSTER_INTERRUPT_LINES, 0);
    pmsprintf(entry, sizeof(entry), "%s.%s", "kernel.percpu.interrupts", "line");
    pmdaTreeInsert(interrupt_tree, pmid, entry);
    pmid = pmID_build(domain, CLUSTER_INTERRUPT_OTHER, 0);
    pmsprintf(entry, sizeof(entry), "%s.%s", "kernel.percpu.interrupts", "none");
    pmdaTreeInsert(interrupt_tree, pmid, entry);

    pmdaTreeRebuildHash(interrupt_tree, 2);
    return interrupt_tree;
}

static void
update_softirqs_pmns(int domain, const char *name)
{
    char entry[128];
    unsigned int item = dynamic_item_lookup(name, SOFTIRQS_NAMES_INDOM);
    pmID pmid = pmID_build(domain, CLUSTER_SOFTIRQS, item);

    pmsprintf(entry, sizeof(entry), "%s.%s", "kernel.percpu.softirqs", name);
    pmdaTreeInsert(softirqs_tree, pmid, entry);
}

static pmdaNameSpace *
noop_softirqs_pmns(int domain)
{
    char entry[128];
    pmID pmid = pmID_build(domain, CLUSTER_SOFTIRQS, 0);

    pmsprintf(entry, sizeof(entry), "%s.%s", "kernel.percpu.softirqs", "none");
    pmdaTreeInsert(softirqs_tree, pmid, entry);
    pmdaTreeRebuildHash(softirqs_tree, 1);
    return softirqs_tree;
}

static int
map_online_cpus(char *buffer)
{
    unsigned int i = 0, cpuid;
    char *s, *end;

    for (s = buffer; i < _pm_ncpus && *s != '\0'; s++) {
	if (!isdigit((int)*s))
	    continue;
	cpuid = (unsigned int)strtoul(s, &end, 10);
	if (end == s)
	    break;
	online_cpumap[i++].cpuid = cpuid;
	s = end;
    }
    return i;
}

static int
column_to_cpuid(int column)
{
    int i;

    if (online_cpumap[column].cpuid == column)
	return column;
    for (i = 0; i < cpu_count; i++)
	if (online_cpumap[i].cpuid == column)
	    return i;
    return 0;
}

static char *
extract_values(char *buffer, interrupt_t *ip, int ncolumns, int count)
{
    unsigned long i, cpuid, value;
    char *s = buffer, *end = NULL;

    ip->total = 0;
    for (i = 0; i < ncolumns; i++) {
	value = strtoul(s, &end, 10);
	if (!isspace(*end))
	    return NULL;
	s = end;
	cpuid = column_to_cpuid(i);
	if (count)
	    online_cpumap[cpuid].count += value;
	ip->values[cpuid] = value;
	ip->total += value;
    }
    return end;
}

/* Create oneline help text - remove duplicates and end-of-line marker */
static char *
oneline_reformat(char *buf)
{
    char *result, *start, *end;

    /* position end marker, and skip over whitespace at the start */
    for (start = end = buf; *end != '\n' && *end != '\0'; end++)
	if (isspace((int)*start) && isspace((int)*end))
	    start = end+1;
    *end = '\0';

    /* squash duplicate whitespace and remove trailing whitespace */
    for (result = start; *result != '\0'; result++) {
	if (isspace((int)result[0]) && (isspace((int)result[1]) || result[1] == '\0')) {
	    memmove(&result[0], &result[1], end - &result[0]);
	    result--;
	}
    }
    return start;
}

static void
reset_indom_cache(int indom, interrupt_t *data, int count)
{
    int i;
    pmInDom cache = INDOM(indom);

    for (i = 0; i < count; i++) {
	interrupt_t *ip = &data[i];
	pmdaCacheStore(cache, PMDA_CACHE_ADD, ip->name, (void **)ip);
    }
    pmdaCacheOp(cache, PMDA_CACHE_SAVE);
}

static void
initialise_interrupt(interrupt_t *ip, unsigned int id, char *s, char *end)
{
    ip->id = id;
    ip->name = strdup(s);
    ip->text = end ? strdup(oneline_reformat(end)) : NULL;
}

static void
initialise_named_interrupt(interrupt_t *ip, int cache, char *name, char *end)
{
    ip->id = dynamic_name_insert(name, cache, (void *)ip);
    ip->name = dynamic_name_lookup(ip->id, cache);
    ip->text = end ? strdup(oneline_reformat(end)) : NULL;
}

static int
extend_interrupts(interrupt_t **interp, int indom, unsigned int *countp)
{
    int cnt = cpu_count * sizeof(unsigned long);
    unsigned long *values = malloc(cnt);
    interrupt_t *ip, *interrupt = *interp;
    int count = *countp + 1;

    if (!values)
	return 0;

    ip = realloc(interrupt, count * sizeof(interrupt_t));
    if (!ip) {
	pmdaCacheOp(INDOM(indom), PMDA_CACHE_CULL);
	free(values);
	return 0;
    }
    interrupt = ip;
    interrupt[count-1].values = values;
    *interp = interrupt;
    *countp = count;
    return 1;
}

static char *
extract_interrupt_name(char *buffer, char **suffix)
{
    char *s = buffer, *end;

    while (isspace((int)*s))		/* find start of name */
	s++;
    for (end = s; *end && !isspace((int)*end); end++) {
	if (!isalnum((int)*end))	/* check valid PMNS entry here; */
	    *end = '_';			/* e.g. s390x has an "I/O" line */
    }
    if (*(end-1) == '_')		/* overwrite final non-name char */
	*(--end) = '\0';		/* and then mark end of name */
    else
	*end = '\0';			/* mark end of name */
    *suffix = end + 1;			/* mark values start */
    return s;
}

static int
extract_interrupt_lines(char *buffer, int ncolumns, int nlines)
{
    unsigned long id;
    char *name, *end, *values;
    int resize = (nlines >= lines_count);

    name = extract_interrupt_name(buffer, &values);
    id = strtoul(name, &end, 10);
    if (*end != '\0')
	return 0;
    if (resize &&
	!extend_interrupts(&interrupt_lines, INTERRUPTS_INDOM, &lines_count))
	return 0;
    end = extract_values(values, &interrupt_lines[nlines], ncolumns, 1);
    if (resize) {
	initialise_interrupt(&interrupt_lines[nlines], id, name, end);
	reset_indom_cache(INTERRUPTS_INDOM, interrupt_lines, nlines+1);
	return 2;
    }
    return 1;
}

static int
extract_interrupt_errors(char *buffer)
{
    return (sscanf(buffer, " ERR: %u", &irq_err_count) == 1 ||
	    sscanf(buffer, "Err: %u", &irq_err_count) == 1  ||
	    sscanf(buffer, "BAD: %u", &irq_err_count) == 1);
}

static int
extract_interrupt_misses(char *buffer)
{
    unsigned int irq_mis_count;	/* not exported */
    return sscanf(buffer, " MIS: %u", &irq_mis_count) == 1;
}

static int
extract_interrupt_other(char *buffer, int ncolumns, int nlines)
{
    char *name, *end, *values;
    int resize = (nlines >= other_count);

    name = extract_interrupt_name(buffer, &values);
    if (resize &&
	!extend_interrupts(&interrupt_other, INTERRUPTS_INDOM, &other_count))
	return 0;
    end = extract_values(values, &interrupt_other[nlines], ncolumns, 1);
    if (resize) {
	initialise_named_interrupt(&interrupt_other[nlines],
				   INTERRUPT_NAMES_INDOM, name, end);
	reset_indom_cache(INTERRUPTS_INDOM, interrupt_other, nlines+1);
	return 2;
    }
    return 1;
}

static int
extract_softirqs(char *buffer, int ncolumns, int nlines)
{
    char *name, *end, *values;
    int resize = (nlines >= softirqs_count);

    name = extract_interrupt_name(buffer, &values);
    if (resize &&
	!extend_interrupts(&softirqs, SOFTIRQS_INDOM, &softirqs_count))
	return 0;
    end = extract_values(values, &softirqs[nlines], ncolumns, 0);
    if (resize) {
	initialise_named_interrupt(&softirqs[nlines],
				    SOFTIRQS_NAMES_INDOM, name, end);
	reset_indom_cache(SOFTIRQS_INDOM, softirqs, nlines+1);
	return 2;
    }
    return 1;
}

int
refresh_interrupt_values(void)
{
    FILE *fp;
    int i, j, ncolumns;
    int sts, resized = 0;

    refresh_interrupt_count++;

    if ((sts = setup_interrupts(1)) < 0)
	return sts;

    if ((fp = linux_statsfile("/proc/interrupts", iobuf, iobufsz)) == NULL)
	return -oserror();

    /* first parse header, which maps online CPU number to column number */
    if (fgets(iobuf, iobufsz, fp)) {
	iobuf[iobufsz - 1] = '\0';
	ncolumns = map_online_cpus(iobuf);
    } else {
	fclose(fp);
	return -EINVAL;		/* unrecognised file format */
    }

    i = j = 0;
    while (fgets(iobuf, iobufsz, fp) != NULL) {
	iobuf[iobufsz - 1] = '\0';
	/* next we parse each interrupt line row (starting with a digit) */
	sts = extract_interrupt_lines(iobuf, ncolumns, i++);
	if (sts > 1)
	    resized++;
	if (sts)
	    continue;
	if (extract_interrupt_errors(iobuf))
	    continue;
	if (extract_interrupt_misses(iobuf))
	    continue;
	/* parse other per-CPU interrupt counter rows (starts non-digit) */
	sts = extract_interrupt_other(iobuf, ncolumns, j++);
	if (sts > 1)
	    resized++;
	if (!sts)
	    break;
    }
    fclose(fp);

    if (resized) {
	dynamic_name_save(INTERRUPT_NAMES_INDOM, interrupt_other, other_count);
	pmdaCacheOp(INDOM(INTERRUPTS_INDOM), PMDA_CACHE_SAVE);
    }

    return 0;
}

int
refresh_softirqs_values(void)
{
    FILE *fp;
    int i = 0, ncolumns;
    int sts, resized = 0;

    refresh_softirqs_count++;

    if ((sts = setup_interrupts(0)) < 0)
	return sts;

    if ((fp = linux_statsfile("/proc/softirqs", iobuf, iobufsz)) == NULL)
	return -oserror();

    /* first parse header, which maps online CPU number to column number */
    if (fgets(iobuf, iobufsz, fp)) {
	iobuf[iobufsz - 1] = '\0';
	ncolumns = map_online_cpus(iobuf);
    } else {
	fclose(fp);
	return -EINVAL;		/* unrecognised file format */
    }

    while (fgets(iobuf, iobufsz, fp) != NULL) {
	iobuf[iobufsz - 1] = '\0';
	/* next we parse each softirqs line */
	sts = extract_softirqs(iobuf, ncolumns, i++);
	if (sts > 1)
	    resized = 1;
	if (sts == 0)
	    break;
    }
    fclose(fp);

    if (resized) {
	dynamic_name_save(SOFTIRQS_NAMES_INDOM, softirqs, softirqs_count);
	pmdaCacheOp(INDOM(SOFTIRQS_INDOM), PMDA_CACHE_SAVE);
    }

    return 0;
}

static int
refresh_interrupts(pmdaExt *pmda, pmdaNameSpace **tree)
{
    int i, sts, dom = pmda->e_domain, lines;

    if (interrupt_tree) {
	*tree = interrupt_tree;
    } else if ((sts = pmdaTreeCreate(&interrupt_tree)) < 0) {
	pmNotifyErr(LOG_ERR, "%s: failed to create interrupt names: %s\n",
			pmGetProgname(), pmErrStr(sts));
	*tree = NULL;
    } else if ((sts = refresh_interrupt_values()) < 0) {
	if (pmDebugOptions.libpmda)
	    fprintf(stderr, "%s: failed to update interrupt values: %s\n",
			pmGetProgname(), pmErrStr(sts));
	*tree = NULL;
    } else {
	if ((lines = lines_count) > MAX_INTERRUPT_LINES)
	    lines = MAX_INTERRUPT_LINES;
	for (i = 0; i < lines; i++)
	    update_lines_pmns(dom, i, interrupt_lines[i].id);
	for (i = 0; i < other_count; i++)
	    update_other_pmns(dom, interrupt_other[i].name);
	*tree = interrupt_tree;
	pmdaTreeRebuildHash(interrupt_tree, lines + other_count);
	return 1;
    }

    if (*tree == NULL) {
	*tree = noop_interrupts_pmns(dom);
	return 1;
    }
    return 0;
}

static int
refresh_softirqs(pmdaExt *pmda, pmdaNameSpace **tree)
{
    int i, sts, dom = pmda->e_domain;

    if (softirqs_tree) {
	*tree = softirqs_tree;
    } else if ((sts = pmdaTreeCreate(&softirqs_tree)) < 0) {
	pmNotifyErr(LOG_ERR, "%s: failed to create softirqs names: %s\n",
			pmGetProgname(), pmErrStr(sts));
	*tree = NULL;
    } else if ((sts = refresh_softirqs_values()) < 0) {
	if (pmDebugOptions.libpmda)
	    fprintf(stderr, "%s: failed to update softirqs values: %s\n",
			pmGetProgname(), pmErrStr(sts));
	*tree = NULL;
    } else {
	for (i = 0; i < softirqs_count; i++)
	    update_softirqs_pmns(dom, softirqs[i].name);
	*tree = softirqs_tree;
	pmdaTreeRebuildHash(softirqs_tree, softirqs_count);
	return 1;
    }

    if (*tree == NULL) {
	*tree = noop_softirqs_pmns(dom);
	return 1;
    }
    return 0;
}

int
interrupts_fetch(int cluster, int item, unsigned int inst, pmAtomValue *atom)
{
    interrupt_t *ip;
    int cpuid, sts;

    if (!refresh_interrupt_count)
	refresh_interrupt_values();

    if (cluster == CLUSTER_INTERRUPTS) {
	if (item == 0) {	/* kernel.all.interrupts.total */
	    pmInDom indom = INDOM(INTERRUPTS_INDOM);
	    sts = pmdaCacheLookup(indom, inst, NULL, (void **)&ip);
	    if (sts < 0)
		return sts;
	    if (sts != PMDA_CACHE_ACTIVE)
	    	return PM_ERR_INST;
	    atom->ull = ip->total;
	    return 1;
	}
	if (item == 3) {	/* kernel.all.interrupts.errors */
	    atom->ul = irq_err_count;
	    return 1;
	}
    }
    if (cluster == CLUSTER_SOFTIRQS_TOTAL) {
	if (item == 0) {	/* kernel.all.softirqs.total */
	    pmInDom indom = INDOM(SOFTIRQS_INDOM);
	    sts = pmdaCacheLookup(indom, inst, NULL, (void **)&ip);
	    if (sts < 0)
		return sts;
	    if (sts != PMDA_CACHE_ACTIVE)
	    	return PM_ERR_INST;
	    atom->ull = ip->total;
	    return 1;
	}
    }

    if (inst >= cpu_count)
	return PM_ERR_INST;

    switch (cluster) {
	case CLUSTER_INTERRUPTS:
	    if (item != 4)
		break;
	    cpuid = column_to_cpuid(inst);
	    atom->ull = online_cpumap[cpuid].count;
	    return 1;
	case CLUSTER_INTERRUPT_LINES:
	    if (!lines_count)
		return 0;
	    if (item > lines_count || item > MAX_INTERRUPT_LINES)
		break;
	    atom->ul = interrupt_lines[item].values[inst];
	    return 1;
	case CLUSTER_INTERRUPT_OTHER:
	    if (!other_count)
		return 0;
	    if (!(ip = dynamic_data_lookup(item, INTERRUPT_NAMES_INDOM)))
		break;
	    atom->ul = ip->values[inst];
	    return 1;
	case CLUSTER_SOFTIRQS:
	    if (!softirqs_count)
		return 0;
	    if (!(ip = dynamic_data_lookup(item, SOFTIRQS_NAMES_INDOM)))
		break;
	    atom->ul = ip->values[inst];
	    return 1;
    }
    return PM_ERR_PMID;
}

/*
 * Create a new metric table entry based on an existing one.
 */
static void
refresh_metrictable(pmdaMetric *source, pmdaMetric *dest, int id)
{
    int domain = pmID_domain(source->m_desc.pmid);
    int cluster = pmID_cluster(source->m_desc.pmid);

    memcpy(dest, source, sizeof(pmdaMetric));
    dest->m_desc.pmid = pmID_build(domain, cluster, id);

    if (pmDebugOptions.libpmda)
	fprintf(stderr, "interrupts refresh_metrictable: (%p -> %p) "
			"metric ID dup: %d.%d.%d -> %d.%d.%d\n",
		source, dest, domain, cluster,
		pmID_item(source->m_desc.pmid), domain, cluster, id);
}

/*
 * Needs to answer the question: how much extra space needs to be
 * allocated in the metric table for (dynamic) interrupt metrics"?  
 * Return value is the number of additional entries/trees needed.
 */
static void
interrupts_metrictable(int *total, int *trees)
{
    int		lines;

    if (!refresh_interrupt_count)
	refresh_interrupt_values();

    if ((lines = lines_count) > MAX_INTERRUPT_LINES)
	lines = MAX_INTERRUPT_LINES;
    if (lines > other_count)
	*trees = lines ? lines : 1;
    else
	*trees = other_count ? other_count : 1;
    *total = 2;	/* lines and other */

    if (pmDebugOptions.libpmda)
	fprintf(stderr, "interrupts size_metrictable: %d total x %d trees\n",
		*total, *trees);
}

static void
softirq_metrictable(int *total, int *trees)
{
    if (!refresh_softirqs_count)
	refresh_softirqs_values();

    *trees = softirqs_count ? softirqs_count : 1;
    *total = 1;	/* softirqs */

    if (pmDebugOptions.libpmda)
	fprintf(stderr, "softirqs size_metrictable: %d total x %d trees\n",
		*total, *trees);
}

static int
interrupts_text(pmdaExt *pmda, pmID pmid, int type, char **buf)
{
    interrupt_t *ip;
    int item = pmID_item(pmid);
    int cluster = pmID_cluster(pmid);
    char *text;

    switch (cluster) {
	case CLUSTER_INTERRUPT_LINES:
	    if (!lines_count)
		return PM_ERR_TEXT;
	    if (item > lines_count || item > MAX_INTERRUPT_LINES)
		return PM_ERR_PMID;
	    text = interrupt_lines[item].text;
	    if (text == NULL || text[0] == '\0')
		return PM_ERR_TEXT;
	    *buf = text;
	    return 0;
	case CLUSTER_INTERRUPT_OTHER:
	    if (!other_count)
		return PM_ERR_TEXT;
	    if (!(ip = dynamic_data_lookup(item, INTERRUPT_NAMES_INDOM)))
		return PM_ERR_PMID;
	    text = ip->text;
	    if (text == NULL || text[0] == '\0')
		return PM_ERR_TEXT;
	    *buf = text;
	    return 0;
	case CLUSTER_SOFTIRQS:
	    if (!softirqs_count)
		return PM_ERR_TEXT;
	    if (!(ip = dynamic_data_lookup(item, SOFTIRQS_NAMES_INDOM)))
		return PM_ERR_PMID;
	    text = ip->text;
	    if (text == NULL || text[0] == '\0')
		return PM_ERR_TEXT;
	    *buf = text;
	    return 0;
    }
    return PM_ERR_PMID;
}

void
interrupts_init(pmdaExt *pmda, pmdaMetric *metrictable, int nmetrics)
{
    int set[] = { CLUSTER_INTERRUPT_LINES, CLUSTER_INTERRUPT_OTHER };
    int soft[] = { CLUSTER_SOFTIRQS };

    pmdaExtDynamicPMNS("kernel.percpu.interrupts",
			set, sizeof(set)/sizeof(int),
			refresh_interrupts, interrupts_text,
			refresh_metrictable, interrupts_metrictable,
			metrictable, nmetrics, pmda);
    pmdaExtDynamicPMNS("kernel.percpu.softirqs",
			soft, sizeof(soft)/sizeof(int),
			refresh_softirqs, interrupts_text,
			refresh_metrictable, softirq_metrictable,
			metrictable, nmetrics, pmda);
}
