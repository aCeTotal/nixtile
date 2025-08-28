/* DIRECT IMMEDIATE REBALANCING: Nuclear option to force equal heights instantly */

/* Force equal height distribution immediately when tiles are moved */
void force_immediate_equal_heights(Monitor *m, int target_column) {
	if (!m || target_column < 0) {
		wlr_log(WLR_ERROR, "[nixtile] IMMEDIATE REBALANCE: Invalid parameters");
		return;
	}
	
	wlr_log(WLR_ERROR, "[nixtile] *** IMMEDIATE NUCLEAR REBALANCE *** Column %d - BYPASSING ALL SYSTEMS", target_column);
	
	/* Count tiles in target column */
	int tile_count = 0;
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
		    c->column_group == target_column) {
			tile_count++;
		}
	}
	
	if (tile_count <= 1) {
		wlr_log(WLR_ERROR, "[nixtile] IMMEDIATE: Column %d has %d tiles, no rebalancing needed", target_column, tile_count);
		return;
	}
	
	wlr_log(WLR_ERROR, "[nixtile] IMMEDIATE: Found %d tiles in column %d - forcing equal distribution", tile_count, target_column);
	
	/* NUCLEAR OPTION: Set all tiles to exactly 1.0f and clear ALL interference */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
		    c->column_group == target_column) {
			
			float old_factor = c->height_factor;
			c->height_factor = 1.0f;
			
			wlr_log(WLR_ERROR, "[nixtile] NUCLEAR: Tile %p column %d: %.3f -> 1.000 (%.1f%% each)", 
			        (void*)c, target_column, old_factor, 100.0f / tile_count);
		}
	}
	
	/* Clear ALL manual resize state that could interfere */
	int workspace = get_current_workspace();
	if (workspace >= 0 && workspace < 9) {
		/* Clear workspace storage for this column */
		Client *storage_c;
		int storage_index = 0;
		wl_list_for_each(storage_c, &clients, link) {
			if (VISIBLEON(storage_c, m) && storage_index < MAX_TILES_PER_WORKSPACE) {
				if (!storage_c->isfloating && !storage_c->isfullscreen && 
				    storage_c->column_group == target_column) {
					m->workspace_height_factors[workspace][storage_index] = 1.0f;
				}
				storage_index++;
			}
		}
		
		/* Clear manual resize flags */
		if (m->workspace_manual_resize_performed && target_column < MAX_COLUMNS) {
			m->workspace_manual_resize_performed[workspace][target_column] = false;
		}
		if (target_column < MAX_COLUMNS) {
			manual_resize_performed[target_column] = false;
		}
		
		wlr_log(WLR_ERROR, "[nixtile] NUCLEAR: Cleared all interference for column %d workspace %d", target_column, workspace);
	}
	
	/* Force immediate workspace state save */
	save_workspace_state();
	
	/* Verify results immediately */
	wlr_log(WLR_ERROR, "[nixtile] *** NUCLEAR VERIFICATION *** Column %d results:", target_column);
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
		    c->column_group == target_column) {
			wlr_log(WLR_ERROR, "[nixtile] NUCLEAR VERIFY: Tile %p height_factor = %.3f (MUST be 1.000)", 
			        (void*)c, c->height_factor);
		}
	}
	
	wlr_log(WLR_ERROR, "[nixtile] *** NUCLEAR REBALANCE COMPLETE *** Column %d: %d tiles at %.1f%% each", 
	        target_column, tile_count, 100.0f / tile_count);
}

/* Force equal heights for ALL columns - complete nuclear option */
void force_immediate_equal_heights_all_columns(Monitor *m) {
	if (!m) {
		wlr_log(WLR_ERROR, "[nixtile] NUCLEAR ALL: NULL monitor");
		return;
	}
	
	wlr_log(WLR_ERROR, "[nixtile] *** NUCLEAR REBALANCE ALL COLUMNS *** - COMPLETE RESET");
	
	/* Get optimal columns for current workspace */
	int optimal_columns = get_workspace_optimal_columns();
	
	/* Force rebalance each column that has tiles */
	for (int col = 0; col < optimal_columns && col < MAX_COLUMNS; col++) {
		int tiles_in_column = 0;
		Client *c;
		wl_list_for_each(c, &clients, link) {
			if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && 
			    c->column_group == col) {
				tiles_in_column++;
			}
		}
		
		if (tiles_in_column > 1) {
			wlr_log(WLR_ERROR, "[nixtile] NUCLEAR ALL: Rebalancing column %d (%d tiles)", col, tiles_in_column);
			force_immediate_equal_heights(m, col);
		}
	}
	
	wlr_log(WLR_ERROR, "[nixtile] *** NUCLEAR ALL COMPLETE *** All columns rebalanced");
}
