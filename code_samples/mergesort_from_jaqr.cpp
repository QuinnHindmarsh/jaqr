#include <cstdint>
#include <cstddef>
#include <cmath>
#include <array>
#include <type_traits>
#include <jaqr/runtime/io.h>
std::int32_t jaqr_sort(std::int32_t* jaqr_array, std::int32_t* jaqr_buffer, std::int32_t jaqr_l, std::int32_t jaqr_r) {
    if ( (jaqr_l >= jaqr_r )) {
        return (0);
    }
    std::int32_t jaqr_mid =  (jaqr_l +  ( std::floor((double)( (jaqr_r - jaqr_l )) / (double)((2))) ) );
    (jaqr_sort(jaqr_array, jaqr_buffer, jaqr_l, jaqr_mid));
    (jaqr_sort(jaqr_array, jaqr_buffer,  (jaqr_mid + (1) ), jaqr_r));
    std::int32_t jaqr_i = jaqr_l;
    std::int32_t jaqr_j =  (jaqr_mid + (1) );
    std::int32_t jaqr_k = jaqr_l;
    while ( ( (jaqr_i <= jaqr_mid ) &&  (jaqr_j <= jaqr_r ) )) {
        if ( ((jaqr_array[jaqr_i]) <= (jaqr_array[jaqr_j]) )) {
            ((jaqr_buffer[jaqr_k]) = (jaqr_array[jaqr_i]));
            (jaqr_i =  (jaqr_i + (1) ));
        }
        else {
            ((jaqr_buffer[jaqr_k]) = (jaqr_array[jaqr_j]));
            (jaqr_j =  (jaqr_j + (1) ));
        }
        (jaqr_k =  (jaqr_k + (1) ));
    }
    while ( (jaqr_i <= jaqr_mid )) {
        ((jaqr_buffer[jaqr_k]) = (jaqr_array[jaqr_i]));
        (jaqr_i =  (jaqr_i + (1) ));
        (jaqr_k =  (jaqr_k + (1) ));
    }
    while ( (jaqr_j <= jaqr_r )) {
        ((jaqr_buffer[jaqr_k]) = (jaqr_array[jaqr_j]));
        (jaqr_j =  (jaqr_j + (1) ));
        (jaqr_k =  (jaqr_k + (1) ));
    }
    for (std::int32_t jaqr_x = jaqr_l;  (jaqr_x <= jaqr_r ); (jaqr_x =  (jaqr_x + (1) ))) {
        ((jaqr_array[jaqr_x]) = (jaqr_buffer[jaqr_x]));
    }
    return (0);
}
int main() {
    std::int32_t jaqr_x;
    (jaqr_runtime::scan(jaqr_x));
    std::int32_t* jaqr_array = (new std::int32_t[jaqr_x]());
    std::int32_t* jaqr_buffer = (new std::int32_t[jaqr_x]());
    for (std::int32_t jaqr_y = (0);  (jaqr_y < jaqr_x ); (jaqr_y =  (jaqr_y + (1) ))) {
        (jaqr_runtime::scan((jaqr_array[jaqr_y])));
    }
    (jaqr_sort(jaqr_array, jaqr_buffer, (0),  (jaqr_x - (1) )));
    for (std::int32_t jaqr_y = (0);  (jaqr_y < jaqr_x ); (jaqr_y =  (jaqr_y + (1) ))) {
        (jaqr_runtime::print((jaqr_array[jaqr_y])));
        (jaqr_runtime::print((' ')));
    }
    (jaqr_runtime::print(('\n')));
}
