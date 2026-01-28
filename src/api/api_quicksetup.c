/*
 *  tvheadend - API quick setup helpers
 *
 *  Copyright (C) 2024
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "access.h"
#include "api.h"
#include "htsmsg.h"
#include "input.h"
#include "service_mapper.h"
#include "input/mpegts/dvb.h"
#include "input/mpegts/mpegts_dvb.h"

#define DNA_HELSINKI_FREQ_HZ 234000000
#define DNA_HELSINKI_SRATE   6900000
#define DNA_HELSINKI_NID     41

static int
dvb_service_has_caids ( service_t *s )
{
  elementary_stream_t *st;

  TAILQ_FOREACH(st, &s->s_components.set_all, es_link) {
    if (st->es_type == SCT_CA)
      return 1;
    if (!LIST_EMPTY(&st->es_caids))
      return 1;
  }
  return 0;
}

static int
dvb_service_is_fta ( service_t *s )
{
  /* Respect services configured to ignore encryption. */
  if (s->s_dvb_forcecaid == 0xffff)
    return 0;
  if (dvb_service_has_caids(s))
    return 0;
  return 1;
}

static int
map_fta_services_only ( dvb_network_t *ln )
{
  mpegts_network_t *mn = (mpegts_network_t *)ln;
  mpegts_mux_t *mm;
  mpegts_service_t *ms;
  service_mapper_conf_t conf;
  int mapped = 0;

  conf = service_mapper_conf.d;
  conf.encrypted = 0;
  conf.check_availability = 0;

  LIST_FOREACH(mm, &mn->mn_muxes, mm_network_link)
    LIST_FOREACH(ms, &mm->mm_services, s_dvb_mux_link) {
      service_t *s = (service_t *)ms;
      int fta = 0;

      if (!s->s_is_enabled(s, 0))
        continue;

      tvh_mutex_lock(&s->s_stream_mutex);
      if ((service_is_tv(s) || service_is_radio(s)) && dvb_service_is_fta(s))
        fta = 1;
      tvh_mutex_unlock(&s->s_stream_mutex);

      if (!fta)
        continue;

      if (service_mapper_process(&conf, s, NULL))
        mapped++;
    }

  return mapped;
}

static dvb_network_t *
dna_helsinki_network_find ( void )
{
  mpegts_network_t *mn;

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link) {
    if (!idnode_is_instance(&mn->mn_id, &dvb_network_dvbc_class))
      continue;
    if (strcmp(tvh_str_default(mn->mn_network_name, ""), "DNA Cable"))
      continue;
    return (dvb_network_t *)mn;
  }
  return NULL;
}

static int
dna_helsinki_autosetup
  ( access_t *perm, htsmsg_t *args, htsmsg_t **resp )
{
  dvb_network_t *ln;
  dvb_mux_t *lm;
  dvb_mux_conf_t dmc;
  mpegts_network_t *mn;
  mpegts_mux_t *mm;
  int mapped = 0;
  int do_map = htsmsg_get_bool_or_default(args, "map", 0);
  char nbuf[UUID_HEX_SIZE];
  char mbuf[UUID_HEX_SIZE];

  (void)perm;

  tvh_mutex_lock(&global_lock);

  ln = dna_helsinki_network_find();
  if (!ln) {
    htsmsg_t *conf = htsmsg_create_map();

    htsmsg_add_bool(conf, "enabled", 1);
    htsmsg_add_str(conf, "networkname", "DNA Cable");
    htsmsg_add_u32(conf, "nid", DNA_HELSINKI_NID);
    /* Allow NIT-based roaming to discover additional muxes. */
    htsmsg_add_s32(conf, "autodiscovery", MN_DISCOVERY_CHANGE);
    htsmsg_add_bool(conf, "ignore_chnum", 0);
    htsmsg_add_bool(conf, "sid_chnum", 0);
    htsmsg_add_bool(conf, "skipinitscan", 0);

    ln = (dvb_network_t *)mpegts_network_build("dvb_network_dvbc", conf);
    htsmsg_destroy(conf);
    if (!ln) {
      tvh_mutex_unlock(&global_lock);
      return EINVAL;
    }
  } else {
    ln->mn_nid = DNA_HELSINKI_NID;
    ln->mn_autodiscovery = MN_DISCOVERY_CHANGE;
    ln->mn_ignore_chnum = 0;
    ln->mn_sid_chnum = 0;
    ln->mn_skipinitscan = 0;
  }

  mn = (mpegts_network_t *)ln;

  dvb_mux_conf_init(mn, &dmc, DVB_SYS_DVBC_ANNEX_A);
  /* DNA Helsinki: 234 MHz / 6900 kSym/s / QAM256 / FEC auto / DVB-C Annex A. */
  dmc.dmc_fe_delsys = DVB_SYS_DVBC_ANNEX_A;
  dmc.dmc_fe_freq = DNA_HELSINKI_FREQ_HZ;
  dmc.dmc_fe_modulation = DVB_MOD_QAM_256;
  dmc.u.dmc_fe_qam.symbol_rate = DNA_HELSINKI_SRATE;
  dmc.u.dmc_fe_qam.fec_inner = DVB_FEC_AUTO;

  mm = (mpegts_mux_t *)dvb_network_find_mux(ln, &dmc,
                                           MPEGTS_ONID_NONE,
                                           MPEGTS_TSID_NONE, 1, 1);
  if (!mm) {
    lm = dvb_mux_create0(ln, MPEGTS_ONID_NONE, MPEGTS_TSID_NONE, &dmc, NULL, NULL);
    mm = mpegts_mux_post_create((mpegts_mux_t *)lm);
  }

  if (mn->mn_scan)
    mn->mn_scan(mn);

  if (do_map && mm)
    mapped = map_fta_services_only(ln);

  idnode_changed(&mn->mn_id);
  if (mm)
    idnode_changed(&mm->mm_id);

  tvh_mutex_unlock(&global_lock);

  *resp = htsmsg_create_map();
  htsmsg_add_str(*resp, "network", idnode_uuid_as_str(&mn->mn_id, nbuf));
  if (mm)
    htsmsg_add_str(*resp, "mux", idnode_uuid_as_str(&mm->mm_id, mbuf));
  htsmsg_add_u32(*resp, "mapped", mapped);

  return 0;
}

static int
api_quicksetup_dna
  ( access_t *perm, void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  return dna_helsinki_autosetup(perm, args, resp);
}

void
api_quicksetup_init ( void )
{
  static const api_hook_t ah[] = {
    { "quicksetup/dna", ACCESS_ADMIN, api_quicksetup_dna, NULL },
    { NULL, 0, NULL, NULL }
  };

  api_register_all(ah);
}
