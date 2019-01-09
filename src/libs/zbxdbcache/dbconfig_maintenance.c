/*
** Zabbix
** Copyright (C) 2001-2019 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/
#include "common.h"
#include "log.h"
#include "zbxalgo.h"
#include "dbcache.h"
#include "mutexs.h"

#define ZBX_DBCONFIG_IMPL
#include "dbconfig.h"

#include "dbsync.h"

extern int		CONFIG_TIMER_FORKS;

/******************************************************************************
 *                                                                            *
 * Function: DCsync_maintenances                                              *
 *                                                                            *
 * Purpose: Updates maintenances in configuration cache                       *
 *                                                                            *
 * Parameters: sync - [IN] the db synchronization data                        *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - maintenanceid                                                *
 *           1 - maintenance_type                                             *
 *           2 - active_since                                                 *
 *           3 - active_till                                                  *
 *           4 - tags_evaltype                                                *
 *                                                                            *
 ******************************************************************************/
void	DCsync_maintenances(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_maintenances";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;
	zbx_uint64_t		maintenanceid;
	zbx_dc_maintenance_t	*maintenance;
	int			found, ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		config->maintenance_update = ZBX_MAINTENANCE_UPDATE_TRUE;

		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(maintenanceid, row[0]);

		maintenance = (zbx_dc_maintenance_t *)DCfind_id(&config->maintenances, maintenanceid,
				sizeof(zbx_dc_maintenance_t), &found);

		if (0 == found)
		{
			maintenance->state = ZBX_MAINTENANCE_IDLE;
			maintenance->running_since = 0;
			maintenance->running_until = 0;

			zbx_vector_uint64_create_ext(&maintenance->groupids, config->maintenances.mem_malloc_func,
					config->maintenances.mem_realloc_func, config->maintenances.mem_free_func);
			zbx_vector_uint64_create_ext(&maintenance->hostids, config->maintenances.mem_malloc_func,
					config->maintenances.mem_realloc_func, config->maintenances.mem_free_func);
			zbx_vector_ptr_create_ext(&maintenance->tags, config->maintenances.mem_malloc_func,
					config->maintenances.mem_realloc_func, config->maintenances.mem_free_func);
			zbx_vector_ptr_create_ext(&maintenance->periods, config->maintenances.mem_malloc_func,
					config->maintenances.mem_realloc_func, config->maintenances.mem_free_func);
		}

		ZBX_STR2UCHAR(maintenance->type, row[1]);
		ZBX_STR2UCHAR(maintenance->tags_evaltype, row[4]);
		maintenance->active_since = atoi(row[2]);
		maintenance->active_until = atoi(row[3]);
	}

	/* remove deleted maintenances */

	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances, &rowid)))
			continue;

		zbx_vector_uint64_destroy(&maintenance->groupids);
		zbx_vector_uint64_destroy(&maintenance->hostids);
		zbx_vector_ptr_destroy(&maintenance->tags);
		zbx_vector_ptr_destroy(&maintenance->periods);

		zbx_hashset_remove_direct(&config->maintenances, maintenance);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_compare_maintenance_tags                                      *
 *                                                                            *
 * Purpose: compare maintenance tags by tag name for sorting                  *
 *                                                                            *
 ******************************************************************************/
static int	dc_compare_maintenance_tags(const void *d1, const void *d2)
{
	const zbx_dc_maintenance_tag_t	*tag1 = *(const zbx_dc_maintenance_tag_t **)d1;
	const zbx_dc_maintenance_tag_t	*tag2 = *(const zbx_dc_maintenance_tag_t **)d2;

	return strcmp(tag1->tag, tag2->tag);
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_maintenance_tags                                          *
 *                                                                            *
 * Purpose: Updates maintenance tags in configuration cache                   *
 *                                                                            *
 * Parameters: sync - [IN] the db synchronization data                        *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - maintenancetagid                                             *
 *           1 - maintenanceid                                                *
 *           2 - operator                                                     *
 *           3 - tag                                                          *
 *           4 - value                                                        *
 *                                                                            *
 ******************************************************************************/
void	DCsync_maintenance_tags(zbx_dbsync_t *sync)
{
	const char			*__function_name = "DCsync_maintenance_tags";

	char				**row;
	zbx_uint64_t			rowid;
	unsigned char			tag;
	zbx_uint64_t			maintenancetagid, maintenanceid;
	zbx_dc_maintenance_tag_t	*maintenance_tag;
	zbx_dc_maintenance_t		*maintenance;
	zbx_vector_ptr_t		maintenances;
	int				found, ret, index, i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&maintenances);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		config->maintenance_update = ZBX_MAINTENANCE_UPDATE_TRUE;

		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(maintenanceid, row[1]);
		if (NULL == (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
				&maintenanceid)))
		{
			continue;
		}

		ZBX_STR2UINT64(maintenancetagid, row[0]);
		maintenance_tag = (zbx_dc_maintenance_tag_t *)DCfind_id(&config->maintenance_tags, maintenancetagid,
				sizeof(zbx_dc_maintenance_tag_t), &found);

		maintenance_tag->maintenanceid = maintenanceid;
		ZBX_STR2UCHAR(maintenance_tag->op, row[2]);
		DCstrpool_replace(found, &maintenance_tag->tag, row[3]);
		DCstrpool_replace(found, &maintenance_tag->value, row[4]);

		if (0 == found)
			zbx_vector_ptr_append(&maintenance->tags, maintenance_tag);

		zbx_vector_ptr_append(&maintenances, maintenance);
	}

	/* remove deleted maintenance tags */

	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (maintenance_tag = (zbx_dc_maintenance_tag_t *)zbx_hashset_search(&config->maintenance_tags,
				&rowid)))
		{
			continue;
		}

		if (NULL != (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
				&maintenance_tag->maintenanceid)))
		{
			index = zbx_vector_ptr_search(&maintenance->tags, &maintenance_tag->maintenancetagid,
					ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

			if (FAIL != index)
				zbx_vector_ptr_remove_noorder(&maintenance->tags, index);

			zbx_vector_ptr_append(&maintenances, maintenance);
		}

		zbx_strpool_release(maintenance_tag->tag);
		zbx_strpool_release(maintenance_tag->value);

		zbx_hashset_remove_direct(&config->maintenance_tags, maintenance_tag);
	}

	/* sort maintenance tags */

	zbx_vector_ptr_sort(&maintenances, ZBX_DEFAULT_PTR_COMPARE_FUNC);
	zbx_vector_ptr_uniq(&maintenances, ZBX_DEFAULT_PTR_COMPARE_FUNC);

	for (i = 0; i < maintenances.values_num; i++)
	{
		maintenance = (zbx_dc_maintenance_t *)maintenances.values[i];
		zbx_vector_ptr_sort(&maintenance->tags, dc_compare_maintenance_tags);
	}

	zbx_vector_ptr_destroy(&maintenances);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_maintenance_periods                                       *
 *                                                                            *
 * Purpose: Updates maintenance period in configuration cache                 *
 *                                                                            *
 * Parameters: sync - [IN] the db synchronization data                        *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - timeperiodid                                                 *
 *           1 - timeperiod_type                                              *
 *           2 - every                                                        *
 *           3 - month                                                        *
 *           4 - dayofweek                                                    *
 *           5 - day                                                          *
 *           6 - start_time                                                   *
 *           7 - period                                                       *
 *           8 - start_date                                                   *
 *           9 - maintenanceid                                                *
 *                                                                            *
 ******************************************************************************/
void	DCsync_maintenance_periods(zbx_dbsync_t *sync)
{
	const char			*__function_name = "DCsync_maintenance_periods";

	char				**row;
	zbx_uint64_t			rowid;
	unsigned char			tag;
	zbx_uint64_t			periodid, maintenanceid;
	zbx_dc_maintenance_period_t	*period;
	zbx_dc_maintenance_t		*maintenance;
	int				found, ret, index;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		config->maintenance_update = ZBX_MAINTENANCE_UPDATE_TRUE;

		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(maintenanceid, row[9]);
		if (NULL == (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
				&maintenanceid)))
		{
			continue;
		}

		ZBX_STR2UINT64(periodid, row[0]);
		period = (zbx_dc_maintenance_period_t *)DCfind_id(&config->maintenance_periods, periodid,
				sizeof(zbx_dc_maintenance_period_t), &found);

		period->maintenanceid = maintenanceid;
		ZBX_STR2UCHAR(period->type, row[1]);
		period->every = atoi(row[2]);
		period->month = atoi(row[3]);
		period->dayofweek = atoi(row[4]);
		period->day = atoi(row[5]);
		period->start_time = atoi(row[6]);
		period->period = atoi(row[7]);
		period->start_date = atoi(row[8]);

		if (0 == found)
			zbx_vector_ptr_append(&maintenance->periods, period);
	}

	/* remove deleted maintenance tags */

	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (period = (zbx_dc_maintenance_period_t *)zbx_hashset_search(&config->maintenance_periods,
				&rowid)))
		{
			continue;
		}

		if (NULL != (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
				&period->maintenanceid)))
		{
			index = zbx_vector_ptr_search(&maintenance->periods, &period->timeperiodid,
					ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

			if (FAIL != index)
				zbx_vector_ptr_remove_noorder(&maintenance->periods, index);
		}

		zbx_hashset_remove_direct(&config->maintenance_periods, period);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_maintenance_groups                                        *
 *                                                                            *
 * Purpose: Updates maintenance groups in configuration cache                 *
 *                                                                            *
 * Parameters: sync - [IN] the db synchronization data                        *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - maintenanceid                                                *
 *           1 - groupid                                                      *
 *                                                                            *
 ******************************************************************************/
void	DCsync_maintenance_groups(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_maintenance_groups";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;
	zbx_dc_maintenance_t	*maintenance = NULL;
	int			index, ret;
	zbx_uint64_t		last_maintenanceid = 0, maintenanceid, groupid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		config->maintenance_update = ZBX_MAINTENANCE_UPDATE_TRUE;

		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(maintenanceid, row[0]);

		if (last_maintenanceid != maintenanceid || 0 == last_maintenanceid)
		{
			if (NULL == (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
					&maintenanceid)))
			{
				continue;
			}
			last_maintenanceid = maintenanceid;
		}

		ZBX_STR2UINT64(groupid, row[1]);

		zbx_vector_uint64_append(&maintenance->groupids, groupid);
	}

	/* remove deleted maintenance groupids from cache */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		ZBX_STR2UINT64(maintenanceid, row[0]);

		if (NULL == (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
				&maintenanceid)))
		{
			continue;
		}
		ZBX_STR2UINT64(groupid, row[1]);

		if (FAIL == (index = zbx_vector_uint64_search(&maintenance->groupids, groupid,
				ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
		{
			continue;
		}

		zbx_vector_uint64_remove_noorder(&maintenance->groupids, index);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: DCsync_maintenance_hosts                                         *
 *                                                                            *
 * Purpose: Updates maintenance hosts in configuration cache                  *
 *                                                                            *
 * Parameters: sync - [IN] the db synchronization data                        *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - maintenanceid                                                *
 *           1 - hostid                                                       *
 *                                                                            *
 ******************************************************************************/
void	DCsync_maintenance_hosts(zbx_dbsync_t *sync)
{
	const char		*__function_name = "DCsync_maintenance_hosts";

	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;
	zbx_vector_ptr_t	maintenances;
	zbx_dc_maintenance_t	*maintenance = NULL;
	int			index, ret, i;
	zbx_uint64_t		last_maintenanceid, maintenanceid, hostid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&maintenances);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		config->maintenance_update = ZBX_MAINTENANCE_UPDATE_TRUE;

		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		ZBX_STR2UINT64(maintenanceid, row[0]);

		if (NULL == maintenance || last_maintenanceid != maintenanceid)
		{
			if (NULL == (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
					&maintenanceid)))
			{
				continue;
			}
			last_maintenanceid = maintenanceid;
		}

		ZBX_STR2UINT64(hostid, row[1]);

		zbx_vector_uint64_append(&maintenance->hostids, hostid);
		zbx_vector_ptr_append(&maintenances, maintenance);
	}

	/* remove deleted maintenance hostids from cache */
	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		ZBX_STR2UINT64(maintenanceid, row[0]);

		if (NULL == (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
				&maintenanceid)))
		{
			continue;
		}
		ZBX_STR2UINT64(hostid, row[1]);

		if (FAIL == (index = zbx_vector_uint64_search(&maintenance->hostids, hostid,
				ZBX_DEFAULT_UINT64_COMPARE_FUNC)))
		{
			continue;
		}

		zbx_vector_uint64_remove_noorder(&maintenance->hostids, index);
		zbx_vector_ptr_append(&maintenances, maintenance);
	}

	zbx_vector_ptr_sort(&maintenances, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);
	zbx_vector_ptr_uniq(&maintenances, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

	for (i = 0; i < maintenances.values_num; i++)
	{
		maintenance = (zbx_dc_maintenance_t *)maintenances.values[i];
		zbx_vector_uint64_sort(&maintenance->hostids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	}

	zbx_vector_ptr_destroy(&maintenances);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_calculate_maintenance_period                                  *
 *                                                                            *
 * Purpose: calculate start time for the specified maintenance period         *
 *                                                                            *
 * Parameter: maintenance   - [IN] the maintenance                            *
 *            period        - [IN] the maintenance period                     *
 *            start_date    - [IN] the period starting timestamp based on     *
 *                                 current time                               *
 *            running_since - [IN] the actual period starting timestamp       *
 *            running_since - [IN] the actual period ending timestamp         *
 *                                                                            *
 * Return value: SUCCEED - a valid period was found                           *
 *               FAIL    - period started before maintenance activation time  *
 *                                                                            *
 ******************************************************************************/
static int	dc_calculate_maintenance_period(const zbx_dc_maintenance_t *maintenance,
		const zbx_dc_maintenance_period_t *period, time_t start_date, time_t *running_since,
		time_t *running_until)
{
	int		day, wday, week;
	struct tm	*tm;
	time_t		active_since = maintenance->active_since;

	if (TIMEPERIOD_TYPE_ONETIME == period->type)
	{
		*running_since = (period->start_date < active_since ? active_since : period->start_date);
		*running_until = period->start_date + period->period;
		if (maintenance->active_until < *running_until)
			*running_until = maintenance->active_until;

		return SUCCEED;
	}

	switch (period->type)
	{
		case TIMEPERIOD_TYPE_DAILY:
			if (start_date < active_since)
				return FAIL;

			tm = localtime(&active_since);
			active_since = active_since - (tm->tm_hour * SEC_PER_HOUR + tm->tm_min * SEC_PER_MIN +
					tm->tm_sec);

			day = (start_date - active_since) / SEC_PER_DAY;
			start_date -= SEC_PER_DAY * (day % period->every);
			break;
		case TIMEPERIOD_TYPE_WEEKLY:
			if (start_date < active_since)
				return FAIL;

			tm = localtime(&active_since);
			wday = (0 == tm->tm_wday ? 7 : tm->tm_wday) - 1;
			active_since = active_since - (wday * SEC_PER_DAY + tm->tm_hour * SEC_PER_HOUR +
					tm->tm_min * SEC_PER_MIN + tm->tm_sec);

			for (; start_date >= active_since; start_date -= SEC_PER_DAY)
			{
				/* check for every x week(s) */
				week = (start_date - active_since) / SEC_PER_WEEK;
				if (0 != week % period->every)
					continue;

				/* check for day of the week */
				tm = localtime(&start_date);
				wday = (0 == tm->tm_wday ? 7 : tm->tm_wday) - 1;
				if (0 == (period->dayofweek & (1 << wday)))
					continue;

				break;
			}
			break;
		case TIMEPERIOD_TYPE_MONTHLY:
			for (; start_date >= active_since; start_date -= SEC_PER_DAY)
			{
				/* check for month */
				tm = localtime(&start_date);
				if (0 == (period->month & (1 << tm->tm_mon)))
					continue;

				if (0 != period->day)
				{
					/* check for day of the month */
					if (period->day != tm->tm_mday)
						continue;
				}
				else
				{
					/* check for day of the week */
					wday = (0 == tm->tm_wday ? 7 : tm->tm_wday) - 1;
					if (0 == (period->dayofweek & (1 << wday)))
						continue;

					/* check for number of day (first, second, third, fourth or last) */
					day = (tm->tm_mday - 1) / 7 + 1;
					if (5 == period->every && 4 == day)
					{
						if (tm->tm_mday + 7 <= zbx_day_in_month(1900 + tm->tm_year,
								tm->tm_mon + 1))
						{
							continue;
						}
					}
					else if (period->every != day)
						continue;
				}

				if (start_date < active_since)
					return FAIL;

				break;
			}
			break;
		default:
			return FAIL;
	}

	*running_since = start_date;
	*running_until = start_date + period->period;
	if (maintenance->active_until < *running_until)
		*running_until = maintenance->active_until;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_maintenance_set_update_flags                              *
 *                                                                            *
 * Purpose: sets maintenance update flags for all timers                      *
 *                                                                            *
 ******************************************************************************/
void	zbx_dc_maintenance_set_update_flags(void)
{
	int	slots_num = ZBX_MAINTENANCE_UPDATE_FLAGS_NUM(), timers_left;

	WRLOCK_CACHE;

	memset(config->maintenance_update_flags, 0xff, sizeof(zbx_uint64_t) * slots_num);

	if (0 != (timers_left = (CONFIG_TIMER_FORKS % (sizeof(uint64_t) * 8))))
		config->maintenance_update_flags[slots_num - 1] >>= (sizeof(zbx_uint64_t) * 8 - timers_left);

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_maintenance_reset_update_flag                             *
 *                                                                            *
 * Purpose: resets maintenance update flags for the specified timer           *
 *                                                                            *
 * Parameters: timer - [IN] the timer process number                          *
 *                                                                            *
 ******************************************************************************/
void	zbx_dc_maintenance_reset_update_flag(int timer)
{
	int		slot, bit;
	zbx_uint64_t	mask;

	timer--;
	slot = timer / (sizeof(uint64_t) * 8);
	bit = timer % (sizeof(uint64_t) * 8);

	mask = ~(__UINT64_C(1) << bit);

	WRLOCK_CACHE;

	config->maintenance_update_flags[slot] &= mask;

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_maintenance_check_update_flag                             *
 *                                                                            *
 * Purpose: checks if the maintenance update flag is set for the specified    *
 *          timer                                                             *
 *                                                                            *
 * Parameters: timer - [IN] the timer process number                          *
 *                                                                            *
 * Return value: SUCCEED - maintenance update flag is set                     *
 *               FAIL    - otherwise                                          *
 ******************************************************************************/
int	zbx_dc_maintenance_check_update_flag(int timer)
{
	int		slot, bit, ret;
	zbx_uint64_t	mask;

	timer--;
	slot = timer / (sizeof(uint64_t) * 8);
	bit = timer % (sizeof(uint64_t) * 8);

	mask = __UINT64_C(1) << bit;

	RDLOCK_CACHE;

	ret = (0 == (config->maintenance_update_flags[slot] & mask) ? FAIL : SUCCEED);

	UNLOCK_CACHE;

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_maintenance_check_update_flags                            *
 *                                                                            *
 * Purpose: checks if at least one maintenance update flag is set             *
 *                                                                            *
 * Return value: SUCCEED - a maintenance update flag is set                   *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dc_maintenance_check_update_flags(void)
{
	int	slots_num = ZBX_MAINTENANCE_UPDATE_FLAGS_NUM(), ret = SUCCEED;

	RDLOCK_CACHE;

	if (0 != config->maintenance_update_flags[0])
		goto out;

	if (1 != slots_num)
	{
		if (0 != memcmp(config->maintenance_update_flags, config->maintenance_update_flags + 1, slots_num - 1))
			goto out;
	}

	ret = FAIL;
out:
	UNLOCK_CACHE;

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_update_maintenances                                       *
 *                                                                            *
 * Purpose: update maintenance state depending on maintenance periods         *
 *                                                                            *
 * Return value: SUCCEED - maintenance status was changed, host/event update  *
 *                         must be performed                                  *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 * Comments: This function calculates if any maintenance period is running    *
 *           and based on that sets current maintenance state - running/idle  *
 *           and period start/end time.                                       *
 *                                                                            *
 ******************************************************************************/
int	zbx_dc_update_maintenances(void)
{
	const char			*__function_name = "zbx_dc_update_maintenances";

	zbx_dc_maintenance_t		*maintenance;
	zbx_dc_maintenance_period_t	*period;
	zbx_hashset_iter_t		iter;
	int				i, running_num = 0, seconds, rc, started_num = 0, stopped_num = 0, ret = FAIL;
	unsigned char			state;
	struct tm			*tm;
	time_t				now, period_start, period_end, running_since, running_until;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	now = time(NULL);
	tm = localtime(&now);
	seconds = tm->tm_hour * SEC_PER_HOUR + tm->tm_min * SEC_PER_MIN + tm->tm_sec;

	WRLOCK_CACHE;

	if (ZBX_MAINTENANCE_UPDATE_TRUE == config->maintenance_update)
	{
		ret = SUCCEED;
		config->maintenance_update = ZBX_MAINTENANCE_UPDATE_FALSE;
	}

	zbx_hashset_iter_reset(&config->maintenances, &iter);
	while (NULL != (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_iter_next(&iter)))
	{
		state = ZBX_MAINTENANCE_IDLE;
		running_since = 0;
		running_until = 0;

		if (now >= maintenance->active_since && now < maintenance->active_until)
		{
			/* find the longest running maintenance period */
			for (i = 0; i < maintenance->periods.values_num; i++)
			{
				period = (zbx_dc_maintenance_period_t *)maintenance->periods.values[i];

				period_start = now - seconds + period->start_time;
				if (seconds < period->start_time)
					period_start -= SEC_PER_DAY;

				rc = dc_calculate_maintenance_period(maintenance, period, period_start, &period_start,
						&period_end);

				if (SUCCEED == rc && period_start <= now && now < period_end)
				{
					state = ZBX_MAINTENANCE_RUNNING;
					if (period_end > running_until)
					{
						running_since = period_start;
						running_until = period_end;
					}
				}
			}
		}

		if (state == ZBX_MAINTENANCE_RUNNING)
		{
			if (ZBX_MAINTENANCE_IDLE == maintenance->state)
			{
				maintenance->running_since = running_since;
				maintenance->state = ZBX_MAINTENANCE_RUNNING;
				started_num++;

				/* Precache nested host groups for started maintenances.   */
				/* Nested host groups for running maintenances are already */
				/* precached during configuration cache synchronization.   */
				for (i = 0; i < maintenance->groupids.values_num; i++)
				{
					zbx_dc_hostgroup_t	*group;

					if (NULL != (group = (zbx_dc_hostgroup_t *)zbx_hashset_search(
							&config->hostgroups, &maintenance->groupids.values[i])))
					{
						dc_hostgroup_cache_nested_groupids(group);
					}
				}
				ret = SUCCEED;
			}

			if (maintenance->running_until != running_until)
			{
				maintenance->running_until = running_until;
				ret = SUCCEED;
			}
			running_num++;
		}
		else
		{
			if (ZBX_MAINTENANCE_RUNNING == maintenance->state)
			{
				maintenance->running_since = 0;
				maintenance->running_until = 0;
				maintenance->state = ZBX_MAINTENANCE_IDLE;
				stopped_num++;
				ret = SUCCEED;
			}
		}
	}

	UNLOCK_CACHE;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() started:%d stopped:%d running:%d", __function_name,
			started_num, stopped_num, running_num);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_get_maintenances_by_ids                                       *
 *                                                                            *
 * Purpose: get maintenances by identifiers                                   *
 *                                                                            *
 ******************************************************************************/
static void	dc_get_maintenances_by_ids(const zbx_vector_uint64_t *maintenanceids, zbx_vector_ptr_t *maintenances)
{
	zbx_dc_maintenance_t	*maintenance;
	int			i;


	for (i = 0; i < maintenanceids->values_num; i++)
	{
		if (NULL != (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_search(&config->maintenances,
				&maintenanceids->values[i])))
		{
			zbx_vector_ptr_append(maintenances, maintenance);
		}

	}
}

/******************************************************************************
 *                                                                            *
 * Function: dc_maintenance_match_host                                        *
 *                                                                            *
 * Purpose: check if the host must be processed by the specified maintenance  *
 *                                                                            *
 * Parameters: maintenance - [IN] the maintenance                             *
 *             hostid      - [IN] identifier of the host to check             *
 *                                                                            *
 * Return value: SUCCEED - the host must be processed by the maintenance      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dc_maintenance_match_host(const zbx_dc_maintenance_t *maintenance, zbx_uint64_t hostid)
{
	int	ret = FAIL;

	if (FAIL != zbx_vector_uint64_bsearch(&maintenance->hostids, hostid, ZBX_DEFAULT_UINT64_COMPARE_FUNC))
		return SUCCEED;

	if (0 != maintenance->groupids.values_num)
	{
		int			i;
		zbx_dc_hostgroup_t	*group;
		zbx_vector_uint64_t	groupids;

		zbx_vector_uint64_create(&groupids);

		for (i = 0; i < maintenance->groupids.values_num; i++)
			dc_get_nested_hostgroupids(maintenance->groupids.values[i], &groupids);

		zbx_vector_uint64_sort(&groupids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
		zbx_vector_uint64_uniq(&groupids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

		for (i = 0; i < groupids.values_num; i++)
		{
			if (NULL == (group = (zbx_dc_hostgroup_t *)zbx_hashset_search(&config->hostgroups,
					&groupids.values[i])))
			{
				continue;
			}

			if (NULL != zbx_hashset_search(&group->hostids, &hostid))
			{
				ret = SUCCEED;
				break;
			}
		}

		zbx_vector_uint64_destroy(&groupids);
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_get_host_maintenance_updates                                  *
 *                                                                            *
 * Purpose: gets maintenance updates for all hosts                            *
 *                                                                            *
 * Parameters: maintenances - [IN] the running maintenances                   *
 *             updates      - [OUT] updates to be applied                     *
 *                                                                            *
 ******************************************************************************/
static void	dc_get_host_maintenance_updates(const zbx_vector_ptr_t *maintenances, zbx_vector_ptr_t *updates)
{
	zbx_hashset_iter_t		iter;
	ZBX_DC_HOST			*host;
	const zbx_dc_maintenance_t	*maintenance;
	int				i, maintenance_from;
	unsigned char			maintenance_status, maintenance_type;
	zbx_uint64_t			maintenanceid;
	zbx_host_maintenance_diff_t	*diff;
	unsigned int			flags;

	zbx_hashset_iter_reset(&config->hosts, &iter);
	while (NULL != (host = (ZBX_DC_HOST *)zbx_hashset_iter_next(&iter)))
	{
		if (HOST_STATUS_PROXY_ACTIVE == host->status || HOST_STATUS_PROXY_PASSIVE == host->status)
			continue;

		maintenance_status = HOST_MAINTENANCE_STATUS_OFF;
		maintenance_type = MAINTENANCE_TYPE_NORMAL;
		maintenanceid = 0;
		maintenance_from = 0;
		flags = 0;

		for (i = 0; i < maintenances->values_num; i++)
		{
			maintenance = (const zbx_dc_maintenance_t *)maintenances->values[i];

			if (SUCCEED == dc_maintenance_match_host(maintenance, host->hostid))
			{
				if (0 == maintenanceid ||
						(MAINTENANCE_TYPE_NORMAL == maintenance_type &&
						MAINTENANCE_TYPE_NODATA == maintenance->type))
				{
					maintenance_status = HOST_MAINTENANCE_STATUS_ON;
					maintenance_type = maintenance->type;
					maintenanceid = maintenance->maintenanceid;
					maintenance_from = maintenance->running_since;
				}
			}
		}

		if (maintenanceid != host->maintenanceid)
			flags |= ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCEID;

		if (maintenance_status != host->maintenance_status)
			flags |= ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCE_STATUS;

		if (maintenance_from != host->maintenance_from)
			flags |= ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCE_FROM;

		if (maintenance_type != host->maintenance_type)
			flags |= ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCE_TYPE;

		if (0 != flags)
		{
			diff = (zbx_host_maintenance_diff_t *)zbx_malloc(0, sizeof(zbx_host_maintenance_diff_t));
			diff->flags = flags;
			diff->hostid = host->hostid;
			diff->maintenanceid = maintenanceid;
			diff->maintenance_status = maintenance_status;
			diff->maintenance_from = maintenance_from;
			diff->maintenance_type = maintenance_type;
			zbx_vector_ptr_append(updates, diff);
		}
	}
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_flush_host_maintenance_updates                            *
 *                                                                            *
 * Purpose: flush host maintenance updates to configuration cache             *
 *                                                                            *
 * Parameters: updates - [IN] the updates to flush                            *
 *                                                                            *
 ******************************************************************************/
void	zbx_dc_flush_host_maintenance_updates(const zbx_vector_ptr_t *updates)
{
	int					i;
	const zbx_host_maintenance_diff_t	*diff;
	ZBX_DC_HOST				*host;

	WRLOCK_CACHE;

	for (i = 0; i < updates->values_num; i++)
	{
		diff = (zbx_host_maintenance_diff_t *)updates->values[i];

		if (NULL == (host = (ZBX_DC_HOST *)zbx_hashset_search(&config->hosts, &diff->hostid)))
			continue;

		if (0 != (diff->flags & ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCEID))
			host->maintenanceid = diff->maintenanceid;

		if (0 != (diff->flags & ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCE_TYPE))
			host->maintenance_type = diff->maintenance_type;

		if (0 != (diff->flags & ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCE_STATUS))
			host->maintenance_status = diff->maintenance_status;

		if (0 != (diff->flags & ZBX_FLAG_HOST_MAINTENANCE_UPDATE_MAINTENANCE_FROM))
			host->maintenance_from = diff->maintenance_from;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_get_host_maintenance_updates                              *
 *                                                                            *
 * Purpose: calculates required host maintenance updates based on specified   *
 *          maintenances                                                      *
 *                                                                            *
 * Parameters: maintenanceids   - [IN] identifiers of the maintenances to     *
 *                                process                                     *
 *             updates          - [OUT] pending updates                       *
 *                                                                            *
 * Comments: This function must be called after zbx_dc_update_maintenances()  *
 *           function has updated maintenance state in configuration cache.   *
 *           To be able to work with lazy nested group caching and read locks *
 *           all nested groups used in maintenances must be already precached *
 *           before calling this function.                                    *
 *                                                                            *
 ******************************************************************************/
void	zbx_dc_get_host_maintenance_updates(const zbx_vector_uint64_t *maintenanceids, zbx_vector_ptr_t *updates)
{
	const char		*__function_name = "zbx_dc_get_host_maintenance_updates";
	zbx_vector_ptr_t	maintenances;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&maintenances);
	zbx_vector_ptr_reserve(&maintenances, 100);

	RDLOCK_CACHE;

	dc_get_maintenances_by_ids(maintenanceids, &maintenances);

	/* host maintenance update must be performed even without running maintenances */
	/* to reset host maintenances status for stopped maintenances                  */
	dc_get_host_maintenance_updates(&maintenances, updates);

	UNLOCK_CACHE;

	zbx_vector_ptr_destroy(&maintenances);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() updates:%d", __function_name, updates->values_num);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_maintenance_tag_match                                         *
 *                                                                            *
 * Purpose: perform maintenance tag comparison using maintenance tag operator *
 *                                                                            *
 ******************************************************************************/
static int	dc_maintenance_tag_value_match(const zbx_dc_maintenance_tag_t *mt, const zbx_tag_t *tag)
{
	switch (mt->op)
	{
		case ZBX_MAINTENANCE_TAG_OPERATOR_LIKE:
			return (NULL != strstr(tag->value, mt->value) ? SUCCEED : FAIL);
		case ZBX_MAINTENANCE_TAG_OPERATOR_EQUAL:
			return (0 == strcmp(tag->value, mt->value) ? SUCCEED : FAIL);
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			return FAIL;
	}
}

/******************************************************************************
 *                                                                            *
 * Function: dc_maintenance_match_tag_range                                   *
 *                                                                            *
 * Purpose: matches tags with [*mt_pos] maintenance tag name                  *
 *                                                                            *
 * Parameters: mtags    - [IN] the maintenance tags, sorted by tag names      *
 *             etags    - [IN] the event tags, sorted by tag names            *
 *             mt_pos   - [IN/OUT] the next maintenance tag index             *
 *             et_pos   - [IN/OUT] the next event tag index                   *
 *                                                                            *
 * Return value: SUCCEED - found matching tag                                 *
 *               FAIL    - no matching tags found                             *
 *                                                                            *
 ******************************************************************************/
static int	dc_maintenance_match_tag_range(const zbx_vector_ptr_t *mtags, const zbx_vector_ptr_t *etags,
		int *mt_pos, int *et_pos)
{
	const zbx_dc_maintenance_tag_t	*mtag;
	const zbx_tag_t			*etag;
	const char			*name;
	int				i, j, ret, mt_start, mt_end, et_start, et_end;

	/* get the maintenance tag name */
	mtag = (const zbx_dc_maintenance_tag_t *)mtags->values[*mt_pos];
	name = mtag->tag;

	/* find maintenance and event tag ranges matching the first maintenance tag name */
	/* (maintenance tag range [mt_start,mt_end], event tag range [et_start,et_end])  */

	mt_start = *mt_pos;
	et_start = *et_pos;

	/* find last maintenance tag with the required name */

	for (i = mt_start + 1; i < mtags->values_num; i++)
	{
		mtag = (const zbx_dc_maintenance_tag_t *)mtags->values[i];
		if (0 != strcmp(mtag->tag, name))
			break;
	}
	mt_end = i - 1;
	*mt_pos = i;

	/* find first event tag with the required name */

	for (i = et_start; i < etags->values_num; i++)
	{
		etag = (const zbx_tag_t *)etags->values[i];
		if (0 < (ret = strcmp(etag->tag, name)))
		{
			*et_pos = i;
			return FAIL;
		}

		if (0 == ret)
			break;
	}

	if (i == etags->values_num)
	{
		*et_pos = i;
		return FAIL;
	}

	et_start = i++;

	/* find last event tag with the required name */

	for (; i < etags->values_num; i++)
	{
		etag = (const zbx_tag_t *)etags->values[i];
		if (0 != strcmp(etag->tag, name))
			break;
	}

	et_end = i - 1;
	*et_pos = i;

	/* cross-compare maintenance and event tags within the found ranges */

	for (i = mt_start; i <= mt_end; i++)
	{
		mtag = (const zbx_dc_maintenance_tag_t *)mtags->values[i];

		for (j = et_start; j <= et_end; j++)
		{
			etag = (const zbx_tag_t *)etags->values[j];
			if (SUCCEED == dc_maintenance_tag_value_match(mtag, etag))
				return SUCCEED;
		}
	}

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_maintenance_match_tags_or                                     *
 *                                                                            *
 * Purpose: matches maintenance and event tags using OR eval type             *
 *                                                                            *
 * Parameters: mtags    - [IN] the maintenance tags, sorted by tag names      *
 *             etags    - [IN] the event tags, sorted by tag names            *
 *                                                                            *
 * Return value: SUCCEED - event tags matches maintenance                     *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dc_maintenance_match_tags_or(const zbx_dc_maintenance_t *maintenance, const zbx_vector_ptr_t *tags)
{
	int	mt_pos = 0, et_pos = 0;

	while (mt_pos < maintenance->tags.values_num && et_pos < tags->values_num)
	{
		if (SUCCEED == dc_maintenance_match_tag_range(&maintenance->tags, tags, &mt_pos, &et_pos))
			return SUCCEED;
	}

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_maintenance_match_tags_andor                                  *
 *                                                                            *
 * Purpose: matches maintenance and event tags using AND/OR eval type         *
 *                                                                            *
 * Parameters: mtags    - [IN] the maintenance tags, sorted by tag names      *
 *             etags    - [IN] the event tags, sorted by tag names            *
 *                                                                            *
 * Return value: SUCCEED - event tags matches maintenance                     *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dc_maintenance_match_tags_andor(const zbx_dc_maintenance_t *maintenance, const zbx_vector_ptr_t *tags)
{
	int	mt_pos = 0, et_pos = 0;

	while (mt_pos < maintenance->tags.values_num && et_pos < tags->values_num)
	{
		if (FAIL == dc_maintenance_match_tag_range(&maintenance->tags, tags, &mt_pos, &et_pos))
			return FAIL;
	}

	if (mt_pos != maintenance->tags.values_num)
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: dc_maintenance_match_tags                                        *
 *                                                                            *
 * Purpose: check if the tags must be processed by the specified maintenance  *
 *                                                                            *
 * Parameters: maintenance - [IN] the maintenance                             *
 *             tags        - [IN] the tags to check                           *
 *                                                                            *
 * Return value: SUCCEED - the tags must be processed by the maintenance      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	dc_maintenance_match_tags(const zbx_dc_maintenance_t *maintenance, const zbx_vector_ptr_t *tags)
{
	switch (maintenance->tags_evaltype)
	{
		case MAINTENANCE_TAG_EVAL_TYPE_AND_OR:
			/* break; is not missing here */
		case MAINTENANCE_TAG_EVAL_TYPE_OR:
			if (0 == maintenance->tags.values_num)
				return SUCCEED;

			if (0 == tags->values_num)
				return FAIL;
			break;
		default:
			THIS_SHOULD_NEVER_HAPPEN;
			return FAIL;
	}

	if (MAINTENANCE_TAG_EVAL_TYPE_AND_OR == maintenance->tags_evaltype)
		return dc_maintenance_match_tags_andor(maintenance, tags);
	else
		return dc_maintenance_match_tags_or(maintenance, tags);
}

/******************************************************************************
 *                                                                            *
 * Function: dc_compare_tags                                                  *
 *                                                                            *
 * Purpose: compare maintenance tags by tag name for sorting                  *
 *                                                                            *
 ******************************************************************************/
static int	dc_compare_tags(const void *d1, const void *d2)
{
	const zbx_tag_t	*tag1 = *(const zbx_tag_t **)d1;
	const zbx_tag_t	*tag2 = *(const zbx_tag_t **)d2;

	return strcmp(tag1->tag, tag2->tag);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_get_event_maintenances                                    *
 *                                                                            *
 * Purpose: get maintenance data for events                                   *
 *                                                                            *
 * Parameters: event_queries -  [IN/OUT] in - event data                      *
 *                                       out - running maintenances for each  *
 *                                            event                           *
 *             maintenanceids - [IN] the maintenances to process              *
 *                                                                            *
 * Return value: SUCCEED - at least one matching maintenance was found        *
 *                                                                            *
 ******************************************************************************/
int	zbx_dc_get_event_maintenances(zbx_vector_ptr_t *event_queries, const zbx_vector_uint64_t *maintenanceids)
{
	const char			*__function_name = "zbx_dc_get_event_maintenances";
	zbx_vector_ptr_t		maintenances;
	int				i, j, k, ret = FAIL;
	zbx_dc_maintenance_t		*maintenance;
	zbx_event_suppress_query_t	*query;
	ZBX_DC_ITEM			*item;
	ZBX_DC_FUNCTION			*function;
	zbx_vector_uint64_t		hostids;
	zbx_uint64_pair_t		pair;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&maintenances);
	zbx_vector_ptr_reserve(&maintenances, 100);
	zbx_vector_uint64_create(&hostids);

	/* event tags must be sorted by name to perform maintenance tag matching */

	for (i = 0; i < event_queries->values_num; i++)
	{
		query = (zbx_event_suppress_query_t *)event_queries->values[i];
		if (0 != query->tags.values_num)
			zbx_vector_ptr_sort(&query->tags, dc_compare_tags);
	}

	RDLOCK_CACHE;

	dc_get_maintenances_by_ids(maintenanceids, &maintenances);

	if (0 == maintenances.values_num)
		goto unlock;

	for (i = 0; i < event_queries->values_num; i++)
	{
		query = (zbx_event_suppress_query_t *)event_queries->values[i];

		/* find hostids of items used in event trigger expressions */

		for (j = 0; j < query->functionids.values_num; j++)
		{
			if (NULL == (function = (ZBX_DC_FUNCTION *)zbx_hashset_search(&config->functions,
					&query->functionids.values[j])))
			{
				continue;
			}

			if (NULL == (item = (ZBX_DC_ITEM *)zbx_hashset_search(&config->items, &function->itemid)))
				continue;

			zbx_vector_uint64_append(&hostids, item->hostid);
		}

		zbx_vector_uint64_sort(&hostids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
		zbx_vector_uint64_uniq(&hostids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

		/* find matching maintenances */
		for (j = 0; j < maintenances.values_num; j++)
		{
			maintenance = (zbx_dc_maintenance_t *)maintenances.values[j];

			if (ZBX_MAINTENANCE_RUNNING != maintenance->state)
				continue;

			for (k = 0; k < hostids.values_num; k++)
			{
				if (SUCCEED == dc_maintenance_match_host(maintenance, hostids.values[k]) &&
						SUCCEED == dc_maintenance_match_tags(maintenance, &query->tags))
				{
					pair.first = maintenance->maintenanceid;
					pair.second = maintenance->running_until;
					zbx_vector_uint64_pair_append(&query->maintenances, pair);
					ret = SUCCEED;
					break;
				}
			}
		}

		zbx_vector_uint64_clear(&hostids);
	}
unlock:
	UNLOCK_CACHE;

	zbx_vector_uint64_destroy(&hostids);
	zbx_vector_ptr_destroy(&maintenances);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_event_suppress_query_free                                    *
 *                                                                            *
 * Purpose: free event suppress query structure                               *
 *                                                                            *
 ******************************************************************************/
void	zbx_event_suppress_query_free(zbx_event_suppress_query_t *query)
{
	zbx_vector_uint64_destroy(&query->functionids);
	zbx_vector_uint64_pair_destroy(&query->maintenances);
	zbx_vector_ptr_clear_ext(&query->tags, (zbx_clean_func_t)zbx_free_tag);
	zbx_vector_ptr_destroy(&query->tags);
	zbx_free(query);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_dc_get_running_maintenanceids                                *
 *                                                                            *
 * Purpose: get identifiers of the running maintenances                       *
 *                                                                            *
 * Return value: SUCCEED - at least one running maintenance was found         *
 *               FAIL    - no running maintenances were found                 *
 *                                                                            *
 ******************************************************************************/
int	zbx_dc_get_running_maintenanceids(zbx_vector_uint64_t *maintenanceids)
{
	zbx_dc_maintenance_t	*maintenance;
	zbx_hashset_iter_t	iter;

	RDLOCK_CACHE;

	zbx_hashset_iter_reset(&config->maintenances, &iter);
	while (NULL != (maintenance = (zbx_dc_maintenance_t *)zbx_hashset_iter_next(&iter)))
	{
		if (ZBX_MAINTENANCE_RUNNING == maintenance->state)
			zbx_vector_uint64_append(maintenanceids, maintenance->maintenanceid);
	}

	UNLOCK_CACHE;

	return (0 != maintenanceids->values_num ? SUCCEED : FAIL);
}

#ifdef HAVE_TESTS
#	include "../../../tests/libs/zbxdbcache/dbconfig_maintenance_test.c"
#endif