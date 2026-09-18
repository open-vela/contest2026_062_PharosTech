/* @nyabula/ui — design system: tokens (mode x palette), motion, icons, base
 * components, global feedback (loading / modal / toast / lightbox).
 * Import '@nyabula/ui/styles/tokens.css', 'base.css' and 'motion.css' in the
 * app entry. Stores require an active Pinia. */
export { default as MdButton } from './components/MdButton.vue';
export { default as MdCard } from './components/MdCard.vue';
export { default as MdChip } from './components/MdChip.vue';
export { default as MdSlider } from './components/MdSlider.vue';
export { default as MdSwitch } from './components/MdSwitch.vue';
export { default as MdTextField } from './components/MdTextField.vue';
export { default as SegmentedTabs } from './components/SegmentedTabs.vue';
export type { SegmentItem } from './components/SegmentedTabs.vue';
export { default as StatusPill } from './components/StatusPill.vue';
export { default as EyeCanvas } from './components/EyeCanvas.vue';
export { default as PairOverlay } from './components/PairOverlay.vue';
export { default as UiIcon } from './components/UiIcon.vue';
export { default as AppLoading } from './components/AppLoading.vue';
export { default as RouteLoading } from './components/RouteLoading.vue';
export { default as AppModal } from './components/AppModal.vue';
export { default as ToastHost } from './components/ToastHost.vue';
export { default as ImageViewer } from './components/ImageViewer.vue';
export type { OriginRect } from './components/ImageViewer.vue';
export { default as ThemeSwitcher } from './components/ThemeSwitcher.vue';
export { default as EmptyState } from './components/EmptyState.vue';
export { default as Skeleton } from './components/Skeleton.vue';
export { default as BottomSheet } from './components/BottomSheet.vue';
export { default as NyabulaLogo } from './components/NyabulaLogo.vue';
export { faviconDataUrl, applyFavicon } from './faviconTemplates';

export { useThemeStore } from './stores/theme';
export { useLoadingStore } from './stores/loading';
export { useDialogStore } from './stores/dialog';
export type { DialogOptions } from './stores/dialog';
export { useToastStore, describeError, ERROR_COPY } from './stores/toast';
export type { Toast, ToastTone } from './stores/toast';

export { circularReveal, eventOrigin } from './theme/circularReveal';
export { derivePalette, mix, onColor, TOKEN_VARS } from './theme/derive';
export { THEME_PRESETS, DEFAULT_PRESET_ID } from './theme/presets';
export type { ThemeMode, ThemePreset, Palette, PaletteColors } from './theme/types';

export { vReveal } from './directives/reveal';
export { UI_ICONS } from './icons';

export { PERMISSION_META, PERMISSION_FALLBACK_ICON, permissionZh, permissionIcon } from './permissions';
export type { PermissionMeta } from './permissions';

/* Feature Kit (Mi Home style consumer components, prefix Nk). See src/kit/README.md. */
export { default as NkHeader } from './kit/NkHeader.vue';
export type { NkHeaderProps } from './kit/NkHeader.vue';
export { default as NkTile } from './kit/NkTile.vue';
export type { NkTileProps } from './kit/NkTile.vue';
export { default as NkStatTile } from './kit/NkStatTile.vue';
export type { NkStatTileProps } from './kit/NkStatTile.vue';
export { default as NkRow } from './kit/NkRow.vue';
export type { NkRowProps } from './kit/NkRow.vue';
export { default as NkToggleRow } from './kit/NkToggleRow.vue';
export type { NkToggleRowProps } from './kit/NkToggleRow.vue';
export { default as NkSliderRow } from './kit/NkSliderRow.vue';
export type { NkSliderRowProps } from './kit/NkSliderRow.vue';
export { default as NkSegmentRow } from './kit/NkSegmentRow.vue';
export type { NkSegmentRowProps } from './kit/NkSegmentRow.vue';
export { default as NkChipSelect } from './kit/NkChipSelect.vue';
export type { NkChipSelectProps, NkChipOption } from './kit/NkChipSelect.vue';
export { default as NkGauge } from './kit/NkGauge.vue';
export type { NkGaugeProps } from './kit/NkGauge.vue';
export { default as NkProgressRing } from './kit/NkProgressRing.vue';
export type { NkProgressRingProps } from './kit/NkProgressRing.vue';
export { default as NkDial } from './kit/NkDial.vue';
export type { NkDialProps, NkDialPreset } from './kit/NkDial.vue';
export { default as NkTimeWheel } from './kit/NkTimeWheel.vue';
export type { NkTimeWheelProps } from './kit/NkTimeWheel.vue';
export { default as NkMediaPlayer } from './kit/NkMediaPlayer.vue';
export type { NkMediaPlayerProps, NkLyricLines } from './kit/NkMediaPlayer.vue';
export { default as NkListSection } from './kit/NkListSection.vue';
export type { NkListSectionProps } from './kit/NkListSection.vue';
export { default as NkActionBar } from './kit/NkActionBar.vue';
export type { NkActionBarProps } from './kit/NkActionBar.vue';
export { default as NkBanner } from './kit/NkBanner.vue';
export type { NkBannerProps } from './kit/NkBanner.vue';
export { default as NkKeyValue } from './kit/NkKeyValue.vue';
export type { NkKeyValueProps, NkKeyValueItem } from './kit/NkKeyValue.vue';
export { default as NkColorSwatch } from './kit/NkColorSwatch.vue';
export type { NkColorSwatchProps } from './kit/NkColorSwatch.vue';
export { default as NkWeatherCard } from './kit/NkWeatherCard.vue';
export type { NkWeatherCardProps, NkWeatherKind } from './kit/NkWeatherCard.vue';
export { default as NkContactList } from './kit/NkContactList.vue';
export type { NkContactListProps, NkContact } from './kit/NkContactList.vue';
