/*
 * DMAP parsing (DAAP)
 *
 * (c) Philippe, philippe_44@outlook.com
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "dmap_parser.h"

// Network byte order conversions for 64-bit
#ifndef ntohll
#define ntohll(x) ((uint64_t)(ntohl((uint32_t)((x) >> 32))) | ((uint64_t)(ntohl((uint32_t)(x))) << 32))
#endif

#define DMAP_VERSION	0x0200
#define DMAP_TIMEOUT	(5*1000)

const static struct {
	char *tag;
	char *desc;
	int datatype;
} dmap_fields[] = {
	{ "abal",	"daap.browsealbumlisting",			TAG_LIST },
	{ "abar",	"daap.browseartistlisting",			TAG_LIST },
	{ "abcp",	"daap.browsecomposerlisting",		TAG_LIST },
	{ "abgn",	"daap.browsegenrelisting",			TAG_LIST },
	{ "abpl",	"daap.baseplaylist",				TAG_BYTE },
	{ "abro",	"daap.databasebrowse",				TAG_LIST },
	{ "adbs",	"daap.databasesongs",				TAG_LIST },
	{ "aeAD",	"com.apple.itunes.adam-ids-array",	TAG_LIST },
	{ "aeAI",	"com.apple.itunes.itms-artistid",	TAG_INT },
	{ "aeCD",	"com.apple.itunes.flat-chapter-data", TAG_DATA },
	{ "aeCF",	"com.apple.itunes.cloud-flavor-id",	TAG_INT },
	{ "aeCI",	"com.apple.itunes.itms-composerid",	TAG_INT },
	{ "aeCK",	"com.apple.itunes.cloud-library-kind", TAG_BYTE },
	{ "aeCM",	"com.apple.itunes.can-be-genius-seed", TAG_BYTE },
	{ "aeCR",	"com.apple.itunes.content-rating",	TAG_STRING },
	{ "aeCS",	"com.apple.itunes.artworkchecksum",	TAG_INT },
	{ "aeCU",	"com.apple.itunes.cloud-user-id",	TAG_LONG },
	{ "aeCd",	"com.apple.itunes.store-catalog-id", TAG_INT },
	{ "aeCI",	"com.apple.itunes.itms-composerid",	TAG_INT },
	{ "aeDP",	"com.apple.itunes.drm-platform-id",	TAG_INT },
	{ "aeDR",	"com.apple.itunes.drm-user-id",		TAG_LONG },
	{ "aeDV",	"com.apple.itunes.drm-versions",	TAG_INT },
	{ "aeEN",	"com.apple.itunes.episode-num-str",	TAG_STRING },
	{ "aeES",	"com.apple.itunes.episode-sort",	TAG_INT },
	{ "aeGD",	"com.apple.itunes.gapless-enc-dr",	TAG_INT },
	{ "aeGE",	"com.apple.itunes.gapless-resy",	TAG_INT },
	{ "aeGH",	"com.apple.itunes.gapless-heur",	TAG_INT },
	{ "aeGI",	"com.apple.itunes.itms-genreid",	TAG_INT },
	{ "aeGR",	"com.apple.itunes.gapless-resy",	TAG_LONG },
	{ "aeGU",	"com.apple.itunes.gapless-dur",		TAG_LONG },
	{ "aeGs",	"com.apple.itunes.can-be-genius-seed", TAG_BYTE },
	{ "aeHC",	"com.apple.itunes.has-chapter-data", TAG_BYTE },
	{ "aeHD",	"com.apple.itunes.is-hd-video",		TAG_BYTE },
	{ "aeHV",	"com.apple.itunes.has-video",		TAG_BYTE },
	{ "aeK1",	"com.apple.itunes.drm-key1-id",		TAG_LONG },
	{ "aeK2",	"com.apple.itunes.drm-key2-id",		TAG_LONG },
	{ "aeMC",	"com.apple.itunes.playlist-contains-media-type", TAG_INT },
	{ "aeMK",	"com.apple.itunes.mediakind",		TAG_BYTE },
	{ "aeMX",	"com.apple.itunes.movie-info-xml",	TAG_STRING },
	{ "aeMk",	"com.apple.itunes.extended-media-kind", TAG_BYTE },
	{ "aeND",	"com.apple.itunes.non-drm-user-id",	TAG_LONG },
	{ "aeNN",	"com.apple.itunes.network-name",	TAG_STRING },
	{ "aeNV",	"com.apple.itunes.norm-volume",		TAG_INT },
	{ "aePC",	"com.apple.itunes.is-podcast",		TAG_BYTE },
	{ "aePP",	"com.apple.itunes.is-podcast-playlist", TAG_BYTE },
	{ "aePS",	"com.apple.itunes.special-playlist", TAG_BYTE },
	{ "aeRD",	"com.apple.itunes.rental-duration", TAG_INT },
	{ "aeRP",	"com.apple.itunes.rental-pb-start", TAG_INT },
	{ "aeRS",	"com.apple.itunes.rental-start",	TAG_INT },
	{ "aeRU",	"com.apple.itunes.rental-pb-duration", TAG_INT },
	{ "aeSE",	"com.apple.itunes.store-pers-id",	TAG_LONG },
	{ "aeSF",	"com.apple.itunes.itms-storefrontid", TAG_INT },
	{ "aeSG",	"com.apple.itunes.saved-genius",	TAG_BYTE },
	{ "aeSI",	"com.apple.itunes.itms-songid",		TAG_INT },
	{ "aeSN",	"com.apple.itunes.series-name",		TAG_STRING },
	{ "aeSP",	"com.apple.itunes.smart-playlist",	TAG_BYTE },
	{ "aeSR",	"com.apple.itunes.sample-rate",		TAG_INT },
	{ "aeSU",	"com.apple.itunes.season-num",		TAG_INT },
	{ "aeSV",	"com.apple.itunes.music-sharing-version", TAG_INT },
	{ "aeXD",	"com.apple.itunes.xid",				TAG_STRING },
	{ "aels",	"com.apple.itunes.liked-state",		TAG_BYTE },
	{ "agrp",	"daap.songgrouping",				TAG_STRING },
	{ "aply",	"daap.databaseplaylists",			TAG_LIST },
	{ "apro",	"daap.protocolversion",				TAG_VERSION },
	{ "apso",	"daap.playlistsongs",				TAG_LIST },
	{ "arif",	"daap.resolveinfo",					TAG_LIST },
	{ "arsv",	"daap.resolve",						TAG_LIST },
	{ "asaa",	"daap.songalbumartist",				TAG_STRING },
	{ "asac",	"daap.songartworkcount",			TAG_SHORT },
	{ "asai",	"daap.songalbumid",					TAG_LONG },
	{ "asal",	"daap.songalbum",					TAG_STRING },
	{ "asar",	"daap.songartist",					TAG_STRING },
	{ "asas",	"daap.songalbumuserratingstatus",	TAG_BYTE },
	{ "asbk",	"daap.bookmarkable",				TAG_BYTE },
	{ "asbo",	"daap.songbookmark",				TAG_INT },
	{ "asbr",	"daap.songbitrate",					TAG_SHORT },
	{ "asbt",	"daap.songbeatsperminute",			TAG_SHORT },
	{ "ascd",	"daap.songcodectype",				TAG_INT },
	{ "ascm",	"daap.songcomment",					TAG_STRING },
	{ "ascn",	"daap.songcontentdescription",		TAG_STRING },
	{ "asco",	"daap.songcompilation",				TAG_BYTE },
	{ "ascp",	"daap.songcomposer",				TAG_STRING },
	{ "ascr",	"daap.songcontentrating",			TAG_BYTE },
	{ "ascs",	"daap.songcodecsubtype",			TAG_INT },
	{ "asct",	"daap.songcategory",				TAG_STRING },
	{ "asda",	"daap.songdateadded",				TAG_DATE },
	{ "asdb",	"daap.songdisabled",				TAG_BYTE },
	{ "asdc",	"daap.songdisccount",				TAG_SHORT },
	{ "asdk",	"daap.songdatakind",				TAG_BYTE },
	{ "asdm",	"daap.songdatemodified",			TAG_DATE },
	{ "asdn",	"daap.songdiscnumber",				TAG_SHORT },
	{ "asdp",	"daap.songdatepurchased",			TAG_DATE },
	{ "asdr",	"daap.songdatereleased",			TAG_DATE },
	{ "asdt",	"daap.songdescription",				TAG_STRING },
	{ "ased",	"daap.songextradata",				TAG_SHORT },
	{ "aseq",	"daap.songeqpreset",				TAG_STRING },
	{ "ases",	"daap.songexcludefromshuffle",		TAG_BYTE },
	{ "asfm",	"daap.songformat",					TAG_STRING },
	{ "asgn",	"daap.songgenre",					TAG_STRING },
	{ "asgp",	"daap.songgapless",					TAG_BYTE },
	{ "asgr",	"daap.supportsgroups",				TAG_BYTE },
	{ "ashp",	"daap.songhasbeenplayed",			TAG_BYTE },
	{ "asky",	"daap.songkeywords",				TAG_STRING },
	{ "aslc",	"daap.songlongcontentdescription",	TAG_STRING },
	{ "aslr",	"daap.songalbumuserrating",			TAG_BYTE },
	{ "asls",	"daap.songlongsize",				TAG_LONG },
	{ "aspc",	"daap.songuserplaycount",			TAG_INT },
	{ "aspl",	"daap.songdateplayed",				TAG_DATE },
	{ "aspu",	"daap.songpodcasturl",				TAG_STRING },
	{ "asri",	"daap.songartistid",				TAG_LONG },
	{ "asrs",	"daap.songuserratingstatus",		TAG_BYTE },
	{ "asrv",	"daap.songrelativevolume",			TAG_BYTE },
	{ "assa",	"daap.sortartist",					TAG_STRING },
	{ "assc",	"daap.sortcomposer",				TAG_STRING },
	{ "assl",	"daap.sortalbumartist",				TAG_STRING },
	{ "assn",	"daap.sortname",					TAG_STRING },
	{ "assp",	"daap.songuserskipcount",			TAG_INT },
	{ "assr",	"daap.songsamplerate",				TAG_INT },
	{ "asss",	"daap.sortseriesname",				TAG_STRING },
	{ "asst",	"daap.songstoptime",				TAG_INT },
	{ "assu",	"daap.sortalbum",					TAG_STRING },
	{ "assz",	"daap.songsize",					TAG_INT },
	{ "astc",	"daap.songtrackcount",				TAG_SHORT },
	{ "astm",	"daap.songtime",					TAG_INT },
	{ "astn",	"daap.songtracknumber",				TAG_SHORT },
	{ "asul",	"daap.songdataurl",					TAG_STRING },
	{ "asur",	"daap.songuserrating",				TAG_BYTE },
	{ "asvc",	"daap.songprimaryvideocodec",		TAG_INT },
	{ "asyr",	"daap.songyear",					TAG_SHORT },
	{ "ated",	"daap.supportsextradata",			TAG_SHORT },
	{ "avdb",	"daap.serverdatabases",				TAG_LIST },
	{ "cafe",	"dacp.fullscreenenabled",			TAG_BYTE },
	{ "cafs",	"dacp.fullscreen",					TAG_BYTE },
	{ "caia",	"dacp.isactive",					TAG_BYTE },
	{ "cana",	"dacp.nowplayingartist",			TAG_STRING },
	{ "cang",	"dacp.nowplayinggenre",				TAG_STRING },
	{ "canl",	"dacp.nowplayingalbum",				TAG_STRING },
	{ "cann",	"dacp.nowplayingtrack",				TAG_STRING },
	{ "canp",	"dacp.nowplayingids",				TAG_LIST },
	{ "cant",	"dacp.remainingtime",				TAG_INT },
	{ "caps",	"dacp.playerstate",					TAG_BYTE },
	{ "carp",	"dacp.repeatstate",					TAG_BYTE },
	{ "cash",	"dacp.shufflestate",				TAG_BYTE },
	{ "casp",	"dacp.speakers",					TAG_LIST },
	{ "cast",	"dacp.tracklength",					TAG_INT },
	{ "casu",	"dacp.su",							TAG_BYTE },
	{ "cave",	"dacp.volumecontrollable",			TAG_BYTE },
	{ "cavc",	"dacp.visualizerenabled",			TAG_BYTE },
	{ "cavs",	"dacp.visualizer",					TAG_BYTE },
	{ "ceGS",	"com.apple.itunes.genius-selectable", TAG_BYTE },
	{ "ceJC",	"com.apple.itunes.jukebox-client-vote", TAG_BYTE },
	{ "ceJI",	"com.apple.itunes.jukebox-current", TAG_INT },
	{ "ceJS",	"com.apple.itunes.jukebox-score",	TAG_INT },
	{ "ceJV",	"com.apple.itunes.jukebox-vote",	TAG_INT },
	{ "ceQR",	"com.apple.itunes.playqueue-contents-response", TAG_LIST },
	{ "ceQa",	"com.apple.itunes.playqueue-album", TAG_STRING },
	{ "ceQg",	"com.apple.itunes.playqueue-genre", TAG_STRING },
	{ "ceQn",	"com.apple.itunes.playqueue-track-name", TAG_STRING },
	{ "ceQr",	"com.apple.itunes.playqueue-artist", TAG_STRING },
	{ "ceSD",	"com.apple.itunes.shuffle-disabled", TAG_BYTE },
	{ "ceSG",	"com.apple.itunes.saved-genius",	TAG_BYTE },
	{ "ceSX",	"com.apple.itunes.music-sharing-version", TAG_INT },
	{ "ceWM",	"com.apple.itunes.wireless-music-services", TAG_LIST },
	{ "cmcp",	"dmcp.controlprompt",				TAG_LIST },
	{ "cmmk",	"dmcp.mediakind",					TAG_INT },
	{ "cmpr",	"dmcp.protocolversion",				TAG_VERSION },
	{ "cmsr",	"dmcp.serverrevision",				TAG_INT },
	{ "cmst",	"dmcp.playstatus",					TAG_LIST },
	{ "cmvo",	"dmcp.volume",						TAG_INT },
	{ "f\215ch",	"dmap.haschildcontainers",		TAG_BYTE },
	{ "ipsa",	"dpap.iphotoslideshowadvancedoptions", TAG_LIST },
	{ "mbcl",	"dmap.bag",							TAG_LIST },
	{ "mccr",	"dmap.contentcodesresponse",		TAG_LIST },
	{ "mcna",	"dmap.contentcodesname",			TAG_STRING },
	{ "mcnm",	"dmap.contentcodesnumber",			TAG_INT },
	{ "mcon",	"dmap.container",					TAG_LIST },
	{ "mctc",	"dmap.containercount",				TAG_INT },
	{ "mcti",	"dmap.containeritemid",				TAG_INT },
	{ "mcty",	"dmap.contentcodestype",			TAG_SHORT },
	{ "mdcl",	"dmap.dictionary",					TAG_LIST },
	{ "meds",	"dmap.editcommandssupported",		TAG_INT },
	{ "meia",	"dmap.itemdateadded",				TAG_DATE },
	{ "meip",	"dmap.itemdateplayed",				TAG_DATE },
	{ "miid",	"dmap.itemid",						TAG_INT },
	{ "mikd",	"dmap.itemkind",					TAG_BYTE },
	{ "mimc",	"dmap.itemcount",					TAG_INT },
	{ "minm",	"dmap.itemname",					TAG_STRING },
	{ "mlcl",	"dmap.listing",						TAG_LIST },
	{ "mlid",	"dmap.sessionid",					TAG_INT },
	{ "mlit",	"dmap.listingitem",					TAG_LIST },
	{ "mlog",	"dmap.loginresponse",				TAG_LIST },
	{ "mpco",	"dmap.parentcontainerid",			TAG_INT },
	{ "mper",	"dmap.persistentid",				TAG_LONG },
	{ "mpro",	"dmap.protocolversion",				TAG_VERSION },
	{ "mrco",	"dmap.returnedcount",				TAG_INT },
	{ "mrpr",	"dmap.remotepersistentid",			TAG_LONG },
	{ "msal",	"dmap.supportsautologout",			TAG_BYTE },
	{ "msas",	"dmap.authenticationschemes",		TAG_INT },
	{ "msau",	"dmap.authenticationmethod",		TAG_BYTE },
	{ "msbr",	"dmap.supportsbrowse",				TAG_BYTE },
	{ "msdc",	"dmap.databasescount",				TAG_INT },
	{ "msed",	"dmap.supportsedit",				TAG_BYTE },
	{ "msex",	"dmap.supportsextensions",			TAG_BYTE },
	{ "msix",	"dmap.supportsindex",				TAG_BYTE },
	{ "mslr",	"dmap.loginrequired",				TAG_BYTE },
	{ "msma",	"dmap.speakerid",					TAG_LONG },
	{ "msml",	"dmap.msml",						TAG_LIST },
	{ "mspi",	"dmap.supportspersistentids",		TAG_BYTE },
	{ "msqy",	"dmap.supportsquery",				TAG_BYTE },
	{ "msrs",	"dmap.supportsresolve",				TAG_BYTE },
	{ "msrv",	"dmap.serverinforesponse",			TAG_LIST },
	{ "mstc",	"dmap.utctime",						TAG_DATE },
	{ "mstm",	"dmap.timeoutinterval",				TAG_INT },
	{ "msto",	"dmap.utcoffset",					TAG_INT },
	{ "msts",	"dmap.statusstring",				TAG_STRING },
	{ "mstt",	"dmap.status",						TAG_INT },
	{ "msup",	"dmap.supportsupdate",				TAG_BYTE },
	{ "msur",	"dmap.serverrevision",				TAG_INT },
	{ "mtco",	"dmap.specifiedtotalcount",			TAG_INT },
	{ "mudl",	"dmap.deletedidlisting",			TAG_LIST },
	{ "mupd",	"dmap.updateresponse",				TAG_LIST },
	{ "musr",	"dmap.serverrevision",				TAG_INT },
	{ "muty",	"dmap.updatetype",					TAG_BYTE },
	{ "pasp",	"dpap.aspectratio",					TAG_STRING },
	{ "pcst",	"daap.podcasturl",					TAG_BYTE },
	{ "peed",	"com.apple.itunes.episode-desc",	TAG_STRING },
	{ "pefs",	"com.apple.itunes.episode-first-seen", TAG_DATE },
	{ "pegr",	"com.apple.itunes.episode-guid-restriction", TAG_BYTE },
	{ "pegs",	"com.apple.itunes.episode-guid",	TAG_STRING },
	{ "pels",	"com.apple.itunes.episode-last-seen", TAG_DATE },
	{ "peod",	"com.apple.itunes.episode-only-download", TAG_BYTE },
	{ "phgt",	"dpap.imagepixelheight",			TAG_INT },
	{ "picd",	"dpap.creationdate",				TAG_DATE },
	{ "pifs",	"dpap.imagefilesize",				TAG_INT },
	{ "pimf",	"dpap.imageformat",					TAG_STRING },
	{ "plsz",	"dpap.imagelargefilesize",			TAG_INT },
	{ "ppro",	"dpap.protocolversion",				TAG_VERSION },
	{ "prat",	"dpap.imagerating",					TAG_INT },
	{ "pret",	"dpap.retryenable",					TAG_BYTE },
	{ "pwth",	"dpap.imagepixelwidth",				TAG_INT },
	{ NULL, 	NULL, 								0 },
};

static int  parse_value(dmap_settings *settings, int type, void *data, int len);

/*----------------------------------------------------------------------------*/
static int get_field(char *tag, int *type)
{
	int i = 0;
	while (dmap_fields[i].tag && strncmp(dmap_fields[i].tag, tag, 4)) i++;
	if (dmap_fields[i].tag) {
		*type = dmap_fields[i].datatype;
		return 1;
	}
	*type = TAG_UNKNOWN;
	return 0;
}

/*----------------------------------------------------------------------------*/
static int parse_container(dmap_settings *settings, char *tag, void *data, int len)
{
	int pos = 0, item_len, item_type;
	char item_tag[5];

	while (len >= 8 && pos <= len - 8) {
		// tag + len
		strncpy(item_tag, (char*) data + pos, 4);
		item_tag[4] = '\0';
		item_len = ntohl(*(uint32_t*)((char*) data + pos + 4));
		if (!get_field(item_tag, &item_type)) {
			if (settings->on_unknown) settings->on_unknown(settings->ctx, item_tag);
			item_type = TAG_UNKNOWN;
		}

		if (pos + 8 + item_len > len) {
			return 0;
		}

		if (item_type == TAG_UNKNOWN) {
			pos += 8 + item_len;
			continue;
		}

		parse_value(settings, item_type, (char*) data + pos + 8, item_len);
		pos += 8 + item_len;
	}

	return 1;
}

/*----------------------------------------------------------------------------*/
static int parse_value(dmap_settings *settings, int type, void *data, int len)
{
	switch (type) {
	case TAG_BYTE:
		if (settings->on_int8) settings->on_int8(settings->ctx, *(uint8_t*)data);
		break;
	case TAG_SHORT:
		if (settings->on_int16) settings->on_int16(settings->ctx, ntohs(*(uint16_t*)data));
		break;
	case TAG_INT:
		if (settings->on_int32) settings->on_int32(settings->ctx, ntohl(*(uint32_t*)data));
		break;
	case TAG_LONG:
		if (settings->on_int64) settings->on_int64(settings->ctx, ntohll(*(uint64_t*)data));
		break;
	case TAG_STRING:
		if (settings->on_string) settings->on_string(settings->ctx, NULL, NULL, (char*) data, len);
		break;
	case TAG_DATE:
		if (settings->on_date) settings->on_date(settings->ctx, ntohl(*(uint32_t*)data));
		break;
	case TAG_VERSION: {
		uint16_t major = ntohs(*(uint16_t*)data);
		uint8_t minor = *((uint8_t*) data + 2);
		uint8_t patch = *((uint8_t*) data + 3);
		if (settings->on_version) settings->on_version(settings->ctx, major, minor, patch);
		break;
	}
	case TAG_LIST:
		parse_container(settings, NULL, data, len);
		break;
	case TAG_DATA:
		if (settings->on_data) settings->on_data(settings->ctx, (unsigned char*) data, len);
		break;
	default:
		break;
	}

	return 1;
}

/*----------------------------------------------------------------------------*/
int dmap_parse(dmap_settings *settings, void *data, int len)
{
	char tag[5] = "";
	int type, size;

	if (len < 8) return 0;

	strncpy(tag, (char*) data, 4);
	tag[4] = '\0';

	if (!get_field(tag, &type)) {
		if (settings->on_unknown) settings->on_unknown(settings->ctx, tag);
		return 0;
	}

	size = ntohl(*(uint32_t*)((char*) data + 4));

	if (size != len - 8) return 0;

	parse_value(settings, type, (char*) data + 8, size);

	return 1;
}