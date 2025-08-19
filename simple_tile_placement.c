/* SIMPLE TILE PLACEMENT LOGIC - GUARANTEED HORIZONTAL PLACEMENT */

/* 
 * This function ensures that the first N tiles (where N = get_optimal_master_tiles())
 * are ALWAYS placed horizontally in separate columns, never stacked.
 * 
 * Rules:
 * 1. Tile 1 -> Column 0
 * 2. Tile 2 -> Column 1  
 * 3. Tile 3 -> Column 2
 * 4. Tile 4+ -> Stack in columns based on preference
 */

int assign_tile_column_simple(Client *new_tile, Monitor *m) {
    if (!new_tile || !m) return 0;
    
    int screen_width = m->w.width;
    int optimal_columns = get_workspace_optimal_columns();
    int master_tiles = get_workspace_nmaster();
    
    /* Count existing tiles in this workspace (excluding the new tile) */
    int existing_tiles = 0;
    Client *c;
    wl_list_for_each(c, &clients, link) {
        if (c != new_tile && VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) {
            existing_tiles++;
        }
    }
    
    int tile_number = existing_tiles + 1; /* 1-based tile number */
    
    wlr_log(WLR_INFO, "[nixtile] SIMPLE PLACEMENT: Tile #%d, master_tiles=%d, optimal_columns=%d", 
        tile_number, master_tiles, optimal_columns);
    
    /* RULE: First N tiles go to separate columns (horizontal placement) */
    if (tile_number <= master_tiles) {
        int target_column = (tile_number - 1) % optimal_columns;
        wlr_log(WLR_INFO, "[nixtile] HORIZONTAL PLACEMENT: Tile #%d -> Column %d (guaranteed horizontal)", 
            tile_number, target_column);
        return target_column;
    }
    
    /* RULE: Remaining tiles stack in columns */
    /* Find column with least tiles for balanced stacking */
    int best_column = 0;
    int min_tiles = 999;
    
    for (int col = 0; col < optimal_columns; col++) {
        int tiles_in_col = count_tiles_in_stack(col, m);
        if (tiles_in_col < min_tiles) {
            min_tiles = tiles_in_col;
            best_column = col;
        }
    }
    
    wlr_log(WLR_INFO, "[nixtile] STACK PLACEMENT: Tile #%d -> Column %d (stacking)", 
        tile_number, best_column);
    
    return best_column;
}
