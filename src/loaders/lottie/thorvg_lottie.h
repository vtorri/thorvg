#ifndef _THORVG_LOTTIE_H_
#define _THORVG_LOTTIE_H_

#include <thorvg.h>

namespace tvg
{

/**
 * @class LottieAnimation
 *
 * @brief The LottieAnimation class enables control of advanced Lottie features.
 *
 * This class extends the Animation and has additional interfaces.
 *
 * @see Animation
 * 
 * @since 0.15
 */
class TVG_API LottieAnimation final : public Animation
{
public:
    /**
     * @brief Override Lottie properties using slot data.
     *
     * @param[in] slot The Lottie slot data in JSON format to override, or @c nullptr to reset.
     *
     * @retval Result::InsufficientCondition In case the animation is not loaded.
     *
     * @since 1.0
     */
    Result override(const char* slot) noexcept;

    /**
    * @brief Specifies a segment by marker. 
    * 
    * Markers are used to control animation playback by specifying start and end points, 
    * eliminating the need to know the exact frame numbers.
    * Generally, markers are designated at the design level, 
    * meaning the callers must know the marker name in advance to use it.
    *
    * @param[in] marker The name of the segment marker.
    *
    * @retval Result::InsufficientCondition If the animation is not loaded.
    * @retval Result::NonSupport When it's not animatable.
    *
    * @note If a @c marker is specified, the previously set segment will be disregarded.
    * @note Set @c nullptr to reset the specified segment.
    * @see Animation::segment(float begin, float end)
    * @since 1.0
    */
    Result segment(const char* marker) noexcept;

    /**
     * @brief Gets the marker count of the animation.
     *
     * @retval The count of the markers, zero if there is no marker.
     * 
     * @see LottieAnimation::marker()
     * @since 1.0
     */
    uint32_t markersCnt() noexcept;
    
    /**
     * @brief Gets the marker name by a given index.
     *
     * @param[in] idx The index of the animation marker, starts from 0.
     *
     * @retval The name of marker when succeed, @c nullptr otherwise.
     * 
     * @see LottieAnimation::markersCnt()
     * @since 1.0
     */
    const char* marker(uint32_t idx) noexcept;

    /**
     * @brief Creates a new LottieAnimation object.
     *
     * @return A new LottieAnimation object.
     *
     * @since 0.15
     */
    static LottieAnimation* gen() noexcept;

    _TVG_DECLARE_PRIVATE(LottieAnimation);
};

} //namespace

#endif //_THORVG_LOTTIE_H_
