/* EQUAL WIDTH DISTRIBUTION: Reset mfact to 0.5 when workspace becomes populated */
/* Count existing visible tiles to determine if workspace was empty */
int existing_tiles = 0;
Client *count_c;
wl_list_for_each(count_c, &clients, link) {
	if (!VISIBLEON(count_c, selmon) || count_c->isfloating || count_c->isfullscreen)
		continue;
	if (count_c != c) /* Don't count the new tile being added */
		existing_tiles++;
}

/* If this is the first or second tile on the workspace, ensure equal width distribution */
if (existing_tiles <= 1) {
	selmon->mfact = 0.5f; /* Always equal 50/50 distribution for new workspaces */
	wlr_log(WLR_DEBUG, "[nixtile] Reset mfact to 0.5 for equal width distribution (existing_tiles=%d)", existing_tiles);
}
